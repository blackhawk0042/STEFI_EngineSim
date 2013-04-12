/*
 * main.c
 */

#include <inc/lm4f120h5qr.h>

#include <inc/hw_memmap.h>
#include <inc/hw_types.h>
#include <inc/hw_ints.h>

#include <driverlib/sysctl.h>
#include <driverlib/pin_map.h>
#include <driverlib/interrupt.h>
#include <driverlib/gpio.h>
#include <driverlib/timer.h>
#include <driverlib/ssi.h>

#include <utils/ustdlib.h>

// SPI hardware
#define SSI					1				// SSI module used for DAC
#define SSI_ADDR_BASE		SSI1_BASE		// Base address of SSI module
#define SSI_FREQ			100000
#define SSI_BIT_WIDTH		8				// Bit-width of SSI comms
#define GPIO_PORT			'D'				// SSI's GPIO port
#define CLK_PIN				GPIO_PIN_0
#define FSS_PIN				GPIO_PIN_1
#define RX_PIN				GPIO_PIN_2
#define TX_PIN				GPIO_PIN_3
#define CLK_PIN_CFG			GPIO_PD0_SSI1CLK
#define FSS_PIN_CFG			GPIO_PD1_SSI1FSS
#define RX_PIN_CFG			GPIO_PD2_SSI1RX
#define TX_PIN_CFG			GPIO_PD3_SSI1TX
#define SSI_INT_MASK		SSI_RXFF

#define LEDS				GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3
#define ENGINE_POS			GPIO_PIN_1
#define FUELINJECT			GPIO_PIN_2

//TODO: Change the start up values
#define StartUp_AMM			100
#define StartUp_O2			150
#define StartUp_WaterTemp	200
#define StartUp_Battery		250
#define StartUp_EnginePos	8000000

void rx_isr (void);
void timer0_isr (void);
void fuelinjectA_isr (void);
void fuelinjectB_isr (void);


unsigned long g_RXData[1];

//Global engine variables (needs to be 8 bit values for msp430)
unsigned long g_AMM;
unsigned long g_O2;
unsigned long g_WaterTemp;
unsigned long g_Battery;
unsigned long g_EnginePos;
//variable for if the engine is on
unsigned char g_EngineOn;

unsigned long g_InjectorTimeOn;
unsigned long g_InjectorTimeOff;



void main (void) {
	unsigned long i;
	// Turn on clock
	SysCtlClockSet(SYSCTL_SYSDIV_4 | SYSCTL_USE_PLL | SYSCTL_OSC_MAIN | SYSCTL_XTAL_16MHZ);

	// Enable clock to SSI module & GPIO port
	SysCtlPeripheralEnable(SYSCTL_PERIPH_SSI1);
	// Char add/subtract allows for easy port switching
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOD);

	// Set pins for use by SSI module
	GPIOPinTypeSSI(GPIO_PORTD_BASE, CLK_PIN | FSS_PIN | TX_PIN | RX_PIN);

	// Set pin MUXes within the SSI module
	GPIOPinConfigure(CLK_PIN_CFG);
	GPIOPinConfigure(FSS_PIN_CFG);
	GPIOPinConfigure(TX_PIN_CFG);
	GPIOPinConfigure(RX_PIN_CFG);

	// Configure tons o' settings for SPI/SSI
	SSIConfigSetExpClk(SSI_ADDR_BASE, SysCtlClockGet(), SSI_FRF_MOTO_MODE_0,
			SSI_MODE_SLAVE, SSI_FREQ, SSI_BIT_WIDTH);

	//SSIIntRegister(SSI_ADDR_BASE, rx_isr);

	SSIEnable(SSI_ADDR_BASE);

	SSIIntEnable(SSI_ADDR_BASE, SSI_INT_MASK);

	// Read in residual data in the FIFO
	while (SSIDataGetNonBlocking(SSI_ADDR_BASE, g_RXData));
	SSIIntClear(SSI_ADDR_BASE, SSI_INT_MASK);

	// Enable GPIO pins for LED
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
	GPIODirModeSet(GPIO_PORTF_BASE, LEDS, GPIO_DIR_MODE_OUT);
	GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, LEDS);

	IntMasterEnable();

	// Setting up Timer0 to be a counter for engine position
	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);
	GPIODirModeSet(GPIO_PORTE_BASE, GPIO_PIN_1, GPIO_DIR_MODE_OUT);
	GPIOPinTypeGPIOOutput(GPIO_PORTE_BASE, GPIO_PIN_1);
	TimerConfigure(TIMER0_BASE, TIMER_CFG_PERIODIC_UP);
	TimerIntRegister(TIMER0_BASE, TIMER_A, timer0_isr);
	IntEnable(INT_TIMER0A);
	TimerIntEnable(TIMER0_BASE, TIMER_TIMA_TIMEOUT);

	// Setting up Timer1 to be fuel injector timer
	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER1);
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
	TimerConfigure(TIMER1_BASE, TIMER_CFG_PERIODIC_UP);
	GPIODirModeSet(GPIO_PORTA_BASE, GPIO_PIN_2, GPIO_DIR_MODE_IN);
	GPIODirModeSet(GPIO_PORTB_BASE, GPIO_PIN_3, GPIO_DIR_MODE_IN);
	GPIOPinTypeGPIOInput(GPIO_PORTA_BASE, GPIO_PIN_2);
	GPIOPinTypeGPIOInput(GPIO_PORTB_BASE, GPIO_PIN_3);
	GPIOIntTypeSet(GPIO_PORTA_BASE, GPIO_PIN_2, GPIO_RISING_EDGE);
	GPIOIntTypeSet(GPIO_PORTB_BASE, GPIO_PIN_3, GPIO_FALLING_EDGE);
	GPIOPortIntRegister(GPIO_PORTA_BASE, fuelinjectA_isr);
	GPIOPortIntRegister(GPIO_PORTB_BASE, fuelinjectB_isr);

	//Sets engine to on state
	//Engine is off = 0
	g_EngineOn = 1;

	if(g_EngineOn == 1)
	{
		g_AMM = StartUp_AMM;
		g_O2 = StartUp_O2;
		g_WaterTemp = StartUp_WaterTemp;
		g_Battery = StartUp_Battery;
		g_EnginePos = StartUp_EnginePos;
		TimerLoadSet(TIMER0_BASE, TIMER_A, g_EnginePos);
		TimerEnable(TIMER0_BASE, TIMER_A);
		GPIOPinIntEnable(GPIO_PORTA_BASE, GPIO_PIN_2);
		GPIOPinIntEnable(GPIO_PORTB_BASE, GPIO_PIN_3);
	}

	while (1) {
		//LED blink; for debug purposes
		GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_3, GPIO_PIN_3);
		for (i = 0; i < 800000; ++i);
		GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_3, 0);
		for (i = 0; i < 800000; ++i);

		/*if(g_EngineOn == 1)
		{
			//TODO: Calculate the g_EnginePos using FI, throttle Pos, and Load

			//Update Timer Match
			TimerMatchSet(TIMER0_BASE, TIMER_A, g_EnginePos);
		}*/

	}
}

void rx_isr (void) {
	unsigned long status = SSIIntStatus(SSI_ADDR_BASE, 1);

	// Blink LED to let us know rx_isr() has been called
	if (GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_1))
		GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, 0);
	else
		GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1);

	// Handle interrupt
	if (SSI_INT_MASK == status)
		SSIDataGet(SSI_ADDR_BASE, g_RXData);

	if (0x01 == g_RXData[0])
	{
		//echos 0x01 as a header
		SSIDataPut(SSI_ADDR_BASE, g_RXData[0]);
		//sends AMM
		SSIDataPut(SSI_ADDR_BASE, g_AMM);
		//sends O2
		SSIDataPut(SSI_ADDR_BASE, g_O2);
	}
	else if (0x02 == g_RXData[0])
	{
		//echos 0x02 as a header
		SSIDataPut(SSI_ADDR_BASE, g_RXData[0]);
		//sends Water Temp
		SSIDataPut(SSI_ADDR_BASE, g_WaterTemp);
		//sends Battery
		SSIDataPut(SSI_ADDR_BASE, g_Battery);
	}
	else
		SSIDataPut(SSI_ADDR_BASE, 0xff);

	SSIIntClear(SSI_ADDR_BASE, status);
}

void timer0_isr (void)
{
	TimerIntClear(TIMER0_BASE, TIMER_TIMA_TIMEOUT);
	//Toggles PE1
	if(GPIOPinRead(GPIO_PORTE_BASE, GPIO_PIN_1))
	{
		GPIOPinWrite(GPIO_PORTE_BASE, GPIO_PIN_1, 0);
	}
	else
	{
		GPIOPinWrite(GPIO_PORTE_BASE, GPIO_PIN_1, GPIO_PIN_1);
	}
	TimerLoadSet(TIMER0_BASE, TIMER_A, g_EnginePos);
}

void fuelinjectA_isr (void)
{
	unsigned long intStatus;
	intStatus = GPIOPinIntStatus(GPIO_PORTA_BASE, 1);
	if(intStatus & GPIO_PIN_2)
	{
		GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_2, GPIO_PIN_2);
		g_InjectorTimeOn = TimerValueGet(TIMER1_BASE, TIMER_A);
		TimerLoadSet(TIMER1_BASE, TIMER_A, 0);
	}
	else
	{
		//error
	}
	GPIOPinIntClear(GPIO_PORTA_BASE, GPIO_PIN_2);
}

void fuelinjectB_isr (void)
{
	unsigned long intStatus;
	intStatus = GPIOPinIntStatus(GPIO_PORTB_BASE, 1);
	if(intStatus & GPIO_PIN_3)
	{
		GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_2, 0);
		g_InjectorTimeOff = TimerValueGet(TIMER1_BASE, TIMER_A);
		TimerLoadSet(TIMER1_BASE, TIMER_A, 0);
	}
	else
	{
		//error
	}
	GPIOPinIntClear(GPIO_PORTB_BASE, GPIO_PIN_3);
}
