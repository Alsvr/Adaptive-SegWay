/*********************************
 * Segway.c
 * Robot balancer adaptive
 * Created: 03/06/2013 22:54:55
 * Author: Juan Lopez Medina
 * Mail: julome0@gmail.com
 ********************************/ 
#define F_CPU			16000000UL	// Frequency XTAL 16MHz

#include <avr/io.h>
#include <avr/interrupt.h>
#include <math.h>
#include <stdlib.h>
#include "usart.h"
#include "twi_master.h"
#include <util/delay.h>

// Constants
#define IMU				0xD0		// Direction Slave IMU
#define DLPF_CFG		0x00		// Low Pass Filter IMU Off
#define GYRO_FS			0x08		// Scale Gyro +/- 500 °/s
#define AFS_SEL			0x18		// Scale Accelerometer a +/- 16g
//#define AFS_SEL		0x00		// Scale Accelerometer a +/- 2g
//#define AFS_SEL		0x08		// Scale Accelerometer a +/- 4g
#define CLKSEL			0x09		// Sleep disable, Clock PLL Gyro_X, temp disable
#define SMPLRT_DIV		0x03		// Sample rate divider at 2 KHz only if DLPF is OFF

// Registries IMU
#define CONFIG			0x1A		// Registry CONFIG
#define GYRO_CONFIG		0x1B		// Registry Gyro
#define ACCEL_CONFIG	0x1C		// Registry Accelerometer
#define ACCEL_XOUT		0x3B		// Registry X_OUT Accelerometer
#define GYRO_XOUT		0x43		// Registry X_OUT Gyro
#define PWR_MGMT_1		0x6B		// Registry Power Management
#define SMPRT_DIV		0x19		// Registry Sample Rate Divider

// Configuration 
#define T_SAMPLE		2			// Time sample Read IMU (Accel and Gyro) T = T_SAMPLE * 1ms
#define T_RESULT		6			// Time sample result Accel + Gyro T = T_RESULT * T_SAMPLE
#define T_CONTROL		1			// Time actuation. Must be synchronized with PWM pulse. T = T_CONTROL *  T_RESULT * T_SAMPLE (this must be less than T_PWM - time algorithm adaptive execution)
#define NL				0.0002		// Noise Level for Adaptive Mechanism.
#define GainA 			0.4			// Gain for Adaptive Mechanism A
#define GainB 			0.2			// Gain for Adaptive Mechanism B
#define PmA				1			// Delay Parameters a
#define PmB				2			// Delay Parameters b
#define hz 5				// Prediction Horizon
#define UP_Roll			800.0		// Upper limit out
#define UP_Yaw			180.0		// Upper limit out
#define GainT_Roll		25.0		// Total Gain Roll Out Controller
#define GainT_Yaw		5.0			// Total Gain Yaw Out Controller
#define MaxOut_Roll		UP_Roll/GainT_Roll
#define MaxOut_Yaw		UP_Yaw/GainT_Yaw


// Calcs
#define GRADE (180.0/M_PI) * 65536.0

// Out PWMs
#define RM PINB1					// Out PWM control Right Motor Timer1 OC1A DPIN 9 Arduino Pro Mini (Atmega328p)
#define LM PINB2					// Out PWM control Left Motor Timer1 OC1B DPIN 10 Arduino Pro Mini (Atmega328p)

// Variables globales
volatile unsigned char cont_PWM = 0;
volatile unsigned char t_sample = 0;		// Count ms for sample timer IMU
volatile unsigned char t_process = 0;		// Enable actuation in a control period
volatile unsigned char t_control = 0;		// Time actuation
volatile unsigned char t_result = 0;		// Time sample result Accel + Gyro (tilt angle result)
long a_gyro[3] = {0, 0, 0};					// Angles x, y, z result from gyroscope
long accel_correct[3] = {0, 0, 0};			// Accelerometer components corrected
double a_result[3] = {0, 0, 0};				// Angles results x, y, z filtered


// Write byte to IMU through TWI
void TWI_Write(unsigned char reg, unsigned char data){  // reg= Direction de registro, data= data to write

	unsigned char messageBuf[8] = {0};					// Buffer for TX through TWI

	messageBuf[0] = IMU;								// TWI slave address (IMU) + Write.
	messageBuf[1] = reg;								// Registry Address to write.
	messageBuf[2] = data;								// Data to Write to IMU.
	TWI_Start_Transceiver_With_Data( messageBuf, 3 );	// TX Reg+Data to Write IMU
}

// Read measurements of IMU Through TWI
void TWI_Read(unsigned char reg, int *result, unsigned char size){
	
	unsigned char messageBuf[8] = {0};					// Buffer for TX through TWI
			 
	messageBuf[0] = IMU;								// TWI slave address (IMU) + Write.
	messageBuf[1] = reg;								// Registry Address to write.	
	TWI_Start_Transceiver_With_Data(messageBuf, 2);		// TX Reg to Write IMU			
	while (TWI_Transceiver_Busy());						// Wait until TWI is ready for next transmission.	
	if (TWI_statusReg.lastTransOK){						// Check if the last operation was successful		
		// Request/collect the data from the Slave
		messageBuf[0] = (IMU | 0x01);					// TWI slave address (IMU) + Read.
		TWI_Start_Transceiver_With_Data(messageBuf, size + 1);				
	} else return;										// Out of function
	while (TWI_Transceiver_Busy());						// Wait until TWI is ready for next transmission.
	if (TWI_statusReg.lastTransOK){						// Check if the last operation was successful		
		TWI_Get_Data_From_Transceiver(messageBuf, size + 1);				
		for (int i = 0; i < (size / 2); i++)				// Get reads 16 bit on array result
		{
			result[i] = (((int)messageBuf[i * 2]) << 8) | (int)messageBuf[(i * 2) + 1];
		}
	}else return; 										// Out of function
}

// Initialize IMU MPU-6050
void imu_init(){
	//TWI_Write(0x68, 0x07);				// Reset all registries				
	TWI_Write(PWR_MGMT_1, CLKSEL);			// Clock selection
	_delay_ms(100);							// Delay 100 ms for init
	TWI_Write(GYRO_CONFIG, GYRO_FS);		// Gyro Scale
	TWI_Write(ACCEL_CONFIG, AFS_SEL);		// Accel Scale
	TWI_Write(CONFIG, DLPF_CFG);			// Config DLPF
	TWI_Write(SMPRT_DIV, SMPLRT_DIV);		// Sample rate divider config											
	return;
}

// Initialize timer2 for system timer (T = 1ms)
void timer2_init(){
	// Prescaler = 128 CTC Mode
	TCCR2A = (1 << WGM21);	
	TCCR2B = (1 << CS22) | (1 << CS20);			
	OCR2A = 124;							// Config end count for T = 1ms		
	TIMSK2 = (1 << OCIE2A);					// Enable CTC interrupt		
	return;
}

// Initialize Timer1 PWM Fast Mode for control Right motors ESC
void timer1_init(){
	//Fast PWM Mode Fpwm = F_cpu / (N * (End + 1))	 	 	 			 
	TCCR1B = (2 << CS10);			// Configuration prescaler to 1/8 N = 8		 	
	ICR1 = 27999;					// End count for Fpwm = 50Hz (20ms) => 39999 for Fpwm = 71.43Hz (14ms) => 27999  		
	TCCR1A = (2 << COM1A0);			// No inverter mode PIN OC1A
	TCCR1A |= (2 << COM1B0);		// No inverter Mode PIN OC1B
									// Enable FAST PWM Mode
	TCCR1A |= (1 << WGM11);
	TCCR1B |= (1 << WGM12) | (1 << WGM13);	 	
	OCR1A = 3000;					// Initialize comparison registry Timer1 PIN A for PWM servo 2000 = 1ms 4000 = 2ms
	OCR1B = 3000;					// Initialize comparison registry Timer1 PIN B for PWM servo 2000 = 1ms 4000 = 2ms
	TIMSK1 |= (1 << TOIE1); 		// Enable interrupt timer1 by ICR1 overflow for reset Sample meters	
	return;
}

// Interrupt CTC TIMER2 for system timer in ms
ISR(TIMER2_COMPA_vect){	
	t_sample ++;		// Increment count ms			
}

// Interrupt timer1 by ICR1 overflow for reset Sample meters	
ISR(TIMER1_OVF_vect){
	//cont_PWM ++;
	//if (cont_PWM == 2){
		//cont_PWM = 0;
		t_process = 1;		// Enable process
		//t_sample = 0;		// Reset t_sample No resetear el angulo del giro sera erroneo
		t_result = 0;		// Reset t_result
		t_control = 0;		// Reset t_control
	//}
}

//Low Pass Filter (2^3) (kf = 0 - 8 kf_min = 1/8 = 0.125)
long lpf(long pv_sf, long pv_fil_ant, signed char kf){
	long pv_fil;
	pv_fil = (kf * pv_sf + (8 - kf) * pv_fil_ant) >> 3;		//pv_fil = kf * pv_sf + (1 - kf) * pv_fil_ant;	
	return pv_fil; 
}	

//Low Pass Filter (2^4) (kf = 0 - 16 kf_min = 1/16 = 0.0625)
long lpf_16(long pv_sf, long pv_fil_ant, signed char kf){
	long pv_fil;
	pv_fil = (kf * pv_sf + (16 - kf) * pv_fil_ant) >> 4;		//pv_fil = kf * pv_sf + (1 - kf) * pv_fil_ant;
	return pv_fil;
}

//Samples Components from Gyro and accelerometer
void sample_meters(unsigned char t)
{
	int gyro[3] = {0, 0, 0};				// Meters axis X, Y, Z Gyroscope
	long gyro_correct[3] = {0, 0, 0};		// Gyroscope components corrected
	static long gyro_correct_ant[3] = {0, 0, 0};			// Gyroscope components corrected k-1		
	int accel[3] = {0, 0, 0};				// Meters axis X, Y, Z accelerometer
	
	// Gyroscope		
	TWI_Read(GYRO_XOUT, gyro, 6);		// Gyro meters		
	//gyro_correct[0] = gyro[1];			// x Axis
	
	if (abs(gyro[1]) > 40) gyro_correct[0] = gyro[1];			// Z Axis comprueba la circuiteria sin trampear
	else gyro_correct[0] = 0;
	
	if (abs(gyro[2]) > 100) gyro_correct[2] = gyro[2];			// Z Axis comprueba la circuiteria sin trampear
	else gyro_correct[2] = 0;
	
	//gyro_correct[2] = gyro[2];			// Z Axis
						
	/*******************************************************
	Angle gyro = w * dt
	w = velocity º/seg
	dt = T * º/seg = gyro_component * T/f_scale_gyro
	angle_gyro = angle_gyro + gyro * dt	
	Tmin = 1ms and f_scale_gyro = +-500 => dt = T * (1ms/65.5) * 2^16 = 1 * T
	********************************************************/	
	a_gyro[0] = a_gyro[0] + t * ((gyro_correct[0] + gyro_correct_ant[0]) >> 1);			//Angle axis X		
	a_gyro[2] = a_gyro[2] + t * ((gyro_correct[2] + gyro_correct_ant[2]) >> 1);			// Angle axis z
	if ((a_gyro[2] > 11796480) | (a_gyro[2] < -11796480)) a_gyro[2] = 0;				//Limits range +180 - -180. If angle > 180 or angle < -180 => angle = 0
		
	//Save k
	gyro_correct_ant[0] = gyro_correct[0];
	gyro_correct_ant[2] = gyro_correct[2];	
		
	// Accelerometer
	TWI_Read(ACCEL_XOUT, accel, 6);			//Accelerometer meters				
	accel_correct[0] = lpf_16(accel[0], accel_correct[0], 1);		// Low pass filter x axis
	accel_correct[2] = lpf_16(accel[2], accel_correct[2], 1);		// Low pass filter z axis	
}

//Tilt Angles calculator arx adding accelerometer	
void angle_result()
{								
	double ax = 0;				//Angles result of accelerometer					
					
	//Angles accelerometer = arctg (x1,x2). Results in radianes (grades = rad * 180/pi) * 2^16 for units as gyro		
	ax = (atan2(accel_correct[0], accel_correct[2])) * GRADE;		//ax = atan2(x, z) * 180/M_PI	
	
	//Filtered recursive complementary (Kalman)
	/*******************************************************
	angle = 0.984 * (angle + gyro * dt) + 0.0156 * angulo_Accel
	0.984 * 2^6 = 63 and 0.0156 * 2^6 = 1
	********************************************************/					
	a_gyro[0] = (63 * a_gyro[0] + 1 * (- ax));		// Update a_gyro adding accelerometer								
	a_gyro[0] = a_gyro[0] >> 6;						// Send update in correct units div by 2^6 for next angle meters	   															
	//a_result[0] = lpf((a_gyro[0] >> 6), a_result[0], 3);				// Result = angle_result/2^16 => angles -180º to 180 	

	a_result[0] = (a_gyro[0] >> 10)/64.0;
	//a_result[2] = lpf((a_gyro[2] >> 6), a_result[2], 5);				// Result = angle_result/2^10 => angles -180º * 2^10 to 180 * 2^10 (YAW)
	a_result[2] = (a_gyro[2] >> 10)/64.0;
}
	
// Adaptive Control
void adaptive(double sp, double *t, double *y, double *u, double *yp, double max_out){

	double y_k = 0;				// Estimated out
	double ek = 0;				// Estimation error
	double q = 0;				// Aux for adaptive process
	double y_dk = 0;			// Incremental Desired Out	
	double y_pdk = 0;			// Process Desired out						
	unsigned int adap;			// Enable/disable  adaptation

			
	y[0] = yp[0] + 180.0;		// Scaled incremental out process for only positive values.
								// Mapped From -90 -- 90 to 0 - 180
									
	// Predictive model order 2 with 4 parameters b					
	y_k = t[0] * y[PmA] + t[1] * y[PmA+1] + t[2] * u[PmB] + t[3] * u[PmB+1] + t[4] * u[PmB+2];		// Estimated out Delay Process = 1
	ek = (y[0] - y_k);						// Estimation error
	
	//if (fabs(ek) < NL) ek = 0.0;				// Noise Level
		//else if (ek > NL) ek = ek - NL;
			//else if (ek < -NL) ek = ek + NL;	
	
	if (fabs(ek) > NL) adap = 1;				// Noise Level
		else adap = 0;
							
	//Adaptive mechanism
	q = adap * ek / (1.0 + (GainA * (pow(y[PmA],2) + pow(y[PmA+1],2)) + GainB * (pow(u[PmB],2) + pow(u[PmB+1],2) + pow(u[PmB+2],2))));
			
	t[0] += (GainA * q * y[PmA]);
	t[1] += (GainA * q * y[PmA+1]);		
	t[2] += (GainB * q * u[PmB]);
	t[3] += (GainB * q * u[PmB+1]);
	t[4] += (GainB * q * u[PmB+2]);
					
	//Desired out calculated. Model ref: a1 = 1, a2 = -0.25, b1 = 0.25	
	//y_pdk = 0.3125 * yp[0] - 0.125 * yp[1] + 0.8125 * sp;		// Prediction horizon = 4 order 2
	//y_pdk = 0.1875 * yp[0] - 0.078125 * yp[1] + 0.890625 * sp;		// Prediction horizon = 5 order 2
	//y_pdk = 0.2128 * yp[0] - 0.081024 * yp[1] + 0.868224 * sp;		// Prediction horizon = 5 order 2 lentooo a2=-0.24
	y_pdk = 0.2387 * yp[0] - 0.083467 * yp[1] + 0.844767 * sp;		// Prediction horizon = 5 order 2 lentooo a2=-0.23

	y_dk = y_pdk + 180.0;											// Scaled for always positive values  
	
	// Calculated Regulator Out 		
	double e1[hz] = { t[0] };	// Vector e1 for horizon
    double e2[hz] = { t[1] };	// vector e2 for horizon    	
	double g1[hz] = { t[2] };	// vector g1 for horizon    
	double g2[hz] = { t[3] };	// vector g2 for horizon    
	double g3[hz] = { t[4] };	// vector g3 for horizon	    	   
	double h = 0;				// h parameter
    
	for (int j = 1; j < hz; j++){
	   	e1[j] = e1[j-1] * e1[0] + e2[j-1];
	   	e2[j] = e1[j-1] * e2[0];	  		
		g1[j] = e1[j-1] * g1[0] + g2[j-1];
		g2[j] = e1[j-1] * g2[0] + g3[j-1];
		g3[j] = e1[j-1] * g3[0];		 						
	}
	for (int j = 0; j < hz; j++){			
		h += g1[j];			
	}		
	u[0] = (y_dk - e1[hz-1] * y[0] - e2[hz-1] * y[1] - g2[hz-1] * u[1] - g3[hz-1] * u[2]) / h;	

	// Upper limit out					
	if (u[0] > max_out) u[0] = max_out;
		else if (u[0] < - max_out) u[0] = - max_out;
	
	//save_process for K+1	
	y[3] = y[2];
	y[2] = y[1];
	y[1] = y[0];
	
	//u[9] = u[8];
	//u[8] = u[7];
	//u[7] = u[6];	
	//u[6] = u[5];
	u[5] = u[4];
	u[4] = u[3];	
	u[3] = u[2];
	u[2] = u[1];
	u[1] = u[0];
	
	yp[1] = yp[0];	
}		
	
int main(void)
{				
	// Initialize
	unsigned char dt = 0;							// Increment time between samples		
	int right_motor = 3000;							// Power for motor right
	int left_motor = 3000;							// Power for motor left
	int out_balancer = 0;							// Out regulator AP control Motor balancer
	int out_yaw = 0;
	double yr[4] = {180 ,180, 180, 180};					// Array out incremental y(k), y(k-1), y(k-2), y(k-3)
	double ur[6] = {0, 0, 0, 0, 0, 0};					// Array process input incremental u(k), u(k-1), u(k-2), u(k-3), u(k-4)
	double tr[5] = {0.49, 0.49, 0.051, 0.041, 0.011};		// Array parameters adaptive mechanism a1k, a2k, b1k, b2k, b3k	
	//double tr[5] = {0, 0, 0, 0.0, 0.0};
	double spr = 0;									// Set point process sp(k)
	double yrp[2] = {0, 0};									// Array process out yp(k)				
	
	double yy[4] = {180 ,180, 180, 180};					// Array out incremental y(k), y(k-1), y(k-2), y(k-3)
	double uy[6] = {0, 0, 0, 0, 0, 0};					// Array process input incremental u(k), u(k-1), u(k-2), u(k-3), u(k-4)
	double ty[5] = {1.0, 0.003, 0.02, 0.012, 0.013};		// Array parameters adaptive mechanism a1k, a2k, b1k, b2k, b3k	
	//double ty[5] = {0, 0, 0, 0, 0};
	double spy = 0;									// Set point process sp(k)
	double yyp[2] = {0, 0};									// Array process out yp(k)					

	DDRB = (1 << RM) | (1 << LM);							// Config output/input pins out motors					
	cli();													// Disable all interrupts
	usart_init();											// Initialize serial port			
	TWI_Master_Initialise();								// Initialize TWI Port
	_delay_ms(10);		
	timer2_init();											// Initialize del timer2 for time system 1ms
	timer1_init();											// Initialize timer 1 for PWM OC1A Right-Motor, OC1B Left-Motor				
	sei();													// Enable global interrupts	
	imu_init();												// Initialize IMU MPU-6050		
	GTCCR |= (1 << TSM) | (1 << PSRASY) | (1 << PSRSYNC);	// Stop and reset Timers
	GTCCR = 0;												// Start Timers synchronized  
		
	// Loop
	while(1)
	{			
		//if (t_process && t_sample >= T_SAMPLE)				// Samples Read IMU (Accel and Gyro)			
		if (t_sample >= T_SAMPLE)
		{			
			dt = t_sample;									// Catch sample time for integer angle gyro
			t_sample = 0;									// Restart sample time			
			sample_meters(dt);								// Samples read IMU									
			t_result++;										// Increment Time sample result
		}
			
		if (t_process && t_result >= T_RESULT)				// Result IMU angles adding accelerometer 	
		{														
			t_result = 0;									// Reset Time sample result																				
			angle_result();									// Result angles meters X,Y,Z 
			t_control++;									// Increment Time actuation
		}		
				
		//Control action balancer	
		if (t_process && t_control >= T_CONTROL){						
			t_control = 0;
			t_process = !t_process;							// Reset t_process													
													
			//Adaptive predictive balancer process
			spr = 0;												// Set point									
			yrp[0] = a_result[0];										// Process out yk. From -180 -- +180 => 180º * 2^7 = 23040						
						
			adaptive(spr, tr, yr, ur, yrp, MaxOut_Roll);							// Call adaptive function												
			out_balancer = ur[0] * GainT_Roll;							// Out Controller adaptive			
			
			if (out_balancer > UP_Roll) out_balancer = UP_Roll;				// Upper limit out
				else if (out_balancer < -UP_Roll) out_balancer = -UP_Roll;
													
			//out_balancer = 0;										// Off control
			
			//Adaptive predictive Yaw process
			spy = 0;												// Set point
			yyp[0] = a_result[2];										// Process out yk. From -180 -- +180 => 180º * 2^7 = 23040
						
			adaptive(spy, ty, yy, uy, yyp, MaxOut_Yaw);							// Call adaptive function
			out_yaw = uy[0] * GainT_Yaw;							// Out Controller adaptive
						
			if (out_yaw > UP_Yaw) out_yaw = UP_Yaw;						// Upper limit out
				else if (out_yaw < -UP_Yaw) out_yaw = -UP_Yaw;
						
			//out_yaw = 0;										// Off control
									
			// Assign control to motors
			right_motor = 3000 + out_balancer + out_yaw;	
			left_motor = 3000 - out_balancer + out_yaw;
					
			//Limits PWM motors
			if (right_motor > 4000) right_motor = 4000;
				else if (right_motor < 2000) right_motor = 2000;										
						
			//Assignments PWMs
			OCR1A =  right_motor;
			OCR1B =  left_motor;			
			/*			
			put_float(tr[0]);
			put_string(" ");
			put_float(tr[1]);
			put_string(" ");
			put_float(tr[2]);
			put_string(" ");
			put_float(tr[3]);
			put_string(" ");
			put_float(tr[4]);				
			put_string("\n");												
			*/
		}									
	}	
}