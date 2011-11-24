// coding: utf-8
// ----------------------------------------------------------------------------
/* Copyright (c) 2011, Roboterclub Aachen e.V.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Roboterclub Aachen e.V. nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ROBOTERCLUB AACHEN E.V. ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL ROBOTERCLUB AACHEN E.V. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
// ----------------------------------------------------------------------------

#include "../gpio.hpp"
#include "i2c_2.hpp"
#include <xpcc/architecture.hpp>

namespace
{
	GPIO__IO(MY_SCL, B, 10);
	GPIO__IO(MY_SDA, B, 11);
	
	static const uint32_t apbId = 12;	// APB2
}

// buffer management
static volatile uint8_t bytes_left;
static volatile uint8_t *readPointer;
static volatile const uint8_t *writePointer;

// parameter advice
static volatile bool reading;
static volatile bool restartAfterReading;
static volatile uint8_t address;

// state
static volatile xpcc::i2c::BusyState busyState = xpcc::i2c::FREE;
static volatile xpcc::i2c::BusState busState;
static volatile bool startConditionGeneratedWaitingForAddress = false;
static volatile bool waitForStartConditionAfterWriteFollowedByRestart = false;

extern "C" void
I2C2_EV_IRQHandler(void)
{
	uint16_t tmp = I2C2->SR1;

	if (tmp & I2C_SR1_SB){ // ev5, start condition generated
		if (!startConditionGeneratedWaitingForAddress){
			// disable interrupts and wait until address and next operation is available
			// while hold a restart must be performed (interface advice), where busState will change to standby
			// while write we are here due to restart, so  bus state has to be changed to standby here
			startConditionGeneratedWaitingForAddress = true;
			I2C2->CR2 &= ~(I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN);
			if(busState != xpcc::i2c::BUS_HOLD)
				busState = xpcc::i2c::BUS_STANDBY;
			busyState = xpcc::i2c::OCCUPIED;
		}
		else{
			// send address
			startConditionGeneratedWaitingForAddress = false;
			I2C2->DR = address | (reading ? xpcc::i2c::READ : xpcc::i2c::WRITE);
		}
	}

	if (tmp & I2C_SR1_ADDR){ // ev6, address is written successfully
		uint16_t tmp2 = I2C2->SR2; // clear addr flag
		if (tmp2 & I2C_SR2_TRA){
			// do nothing because ev8_1 will be automatically triggered
		}
		else{
			if (bytes_left <= 1){
				I2C2->CR1 &= ~I2C_CR1_ACK;
				if (restartAfterReading)
					I2C2->CR1 |= I2C_CR1_START;
				else
					I2C2->CR1 |= I2C_CR1_STOP;
			}
			else{
				I2C2->CR1 |= I2C_CR1_ACK; // for reading more than one byte
			}
		}
	}
	if (tmp & I2C_SR1_TXE){
		if (tmp & I2C_SR1_BTF){
			if(bytes_left == 0){
				// clear other interrupts too, wait for restart or stop or write
				I2C2->CR2 &= ~(I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN);
				busyState = xpcc::i2c::OCCUPIED;
				busState = xpcc::i2c::BUS_WRITE;
			}
			else{// new data available due to repeated write or interrupt underflow happened
				--bytes_left;
				I2C2->DR = *(writePointer++); // write data
				if (bytes_left==0){
					I2C2->CR2 &= ~I2C_CR2_ITBUFEN; // clear interrupt on txe, so wait for btf
				}
			}
		}
		else { //ev8 and ev8_1 transmit is empty
			--bytes_left;
			I2C2->DR = *(writePointer++); // write data
			if (bytes_left==0){
				I2C2->CR2 &= ~I2C_CR2_ITBUFEN; // clear interrupt on txe, so wait for btf
			}
		}
	}

	if (tmp & I2C_SR1_RXNE){// ev7
		uint16_t tmp2 = I2C2->DR; // clear RXNE

		switch (--bytes_left){
			default:
				break;
			case 1://ev7_1
				I2C2->CR1 &= ~I2C_CR1_ACK;
				if (restartAfterReading)
					I2C2->CR1 |= I2C_CR1_START;
				else
					I2C2->CR1 |= I2C_CR1_STOP;
				break;
			case 0:
				if (restartAfterReading){
					busState = xpcc::i2c::BUS_HOLD;
				}
				else{
					busState = xpcc::i2c::BUS_STOPPED;
					busyState = xpcc::i2c::OCCUPIED;
				}
				break;
		}

		*(readPointer++) = tmp2&0xff;
	}
}

extern "C" void
I2C2_ER_IRQHandler(void)
{
	uint16_t tmp = I2C2->SR1;

	if (tmp & I2C_SR1_BERR){
		I2C2->CR1 |= I2C_CR1_STOP;
		busyState = xpcc::i2c::OCCUPIED;
		busState = xpcc::i2c::BUS_RESET;
	}
	if (tmp & I2C_SR1_AF){// acknowledge fail
		I2C2->CR1 |= I2C_CR1_STOP;
		busyState = xpcc::i2c::OCCUPIED;
		busState = xpcc::i2c::BUS_RESET;
	}
	if (tmp & I2C_SR1_ARLO){// arbitration lost
		busyState = xpcc::i2c::OCCUPIED;
		busState = xpcc::i2c::BUS_RESET;
	}

	// overrun error not handled

	// clear flags
	I2C2->SR1 = 0;
	// clear interrupts
	I2C2->CR2 &= ~(I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN);
	waitForStartConditionAfterWriteFollowedByRestart = false;

}


void
xpcc::stm32::I2c2::initialize()
{
	RCC->APB1ENR |= RCC_APB1ENR_I2C2EN;

	MY_SDA::setOutput(xpcc::stm32::ALTERNATE, xpcc::stm32::OPEN_DRAIN);
	MY_SCL::setOutput(xpcc::stm32::ALTERNATE, xpcc::stm32::OPEN_DRAIN);

	I2C2->CR1 = I2C_CR1_SWRST; // reset module
	I2C2->CR1 = 0;

	NVIC_EnableIRQ(I2C2_ER_IRQn);
	NVIC_EnableIRQ(I2C2_EV_IRQn);

	I2C2->CCR = 0xff; // baudrate (prescaler), nur solange i2c disabled, PE = 0
	I2C2->CR2 = 36;// not enable interrupts, 36 MHz ist maximum
//	I2C2->CR2 = I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN | 36;// enable all interrupts, 36 MHz ist maximum
//	I2C2->TRISE = // rise time

	I2C2->CR1 |= I2C_CR1_PE; // enable periferal
}

bool
xpcc::stm32::I2c2::start(uint8_t slaveAddress)
{
	if (busyState == xpcc::i2c::FREE){
		busyState = xpcc::i2c::BUSY;
		address = slaveAddress;

		// don't write anything but clear the flag if it is set
		if (I2C2->SR1 & I2C_SR1_SB){
			I2C2->DR = 0xff;
		}

		I2C2->CR1 |= I2C_CR1_START; // generate a start condition
		waitForStartConditionAfterWriteFollowedByRestart = !(I2C2->CR2 & (I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN)); // for interrupt activation in case they are off

		return true;
	}
	return false;
}

void
xpcc::stm32::I2c2::restart(uint8_t slaveAddress)
{
	address = slaveAddress;
	busyState = xpcc::i2c::BUSY;
	I2C2->CR1 |= I2C_CR1_START; // generate a start condition
	waitForStartConditionAfterWriteFollowedByRestart = !(I2C2->CR2 & (I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN)); // for interrupt activation in case they are off

}

void
xpcc::stm32::I2c2::stop()
{
	if ((busState != xpcc::i2c::BUS_STOPPED && busState != xpcc::i2c::BUS_RESET) ||
			(busState == xpcc::i2c::BUS_STANDBY)){

		I2C2->CR1 |= I2C_CR1_STOP;

		if(startConditionGeneratedWaitingForAddress){
			startConditionGeneratedWaitingForAddress = false;
		}

		// interrupts may be disabled due to write, but do not enable because txe and btf are set during stop condition
	}

	// clear interrupts
	I2C2->CR2 &= ~(I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN);

	busyState = xpcc::i2c::FREE;
}

void
xpcc::stm32::I2c2::read(uint8_t *data, std::size_t size, xpcc::i2c::ReadParameter param)
{
	reading = true;
	readPointer = data;
	bytes_left = size;
	busyState = xpcc::i2c::BUSY;
	restartAfterReading = param == xpcc::i2c::READ_RESTART;

	// enable interrupts again
	I2C2->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN;
}

void
xpcc::stm32::I2c2::write(const uint8_t *data, std::size_t size)
{
	reading = false;
	writePointer = data;
	bytes_left = size;
	busyState = xpcc::i2c::BUSY;

	// enable interrupts again
	I2C2->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN;
}

xpcc::i2c::BusyState
xpcc::stm32::I2c2::getBusyState()
{
	uint16_t tmp = I2C2->CR1;
	if(waitForStartConditionAfterWriteFollowedByRestart && !(tmp & I2C_CR1_START)){
		waitForStartConditionAfterWriteFollowedByRestart = false;
		// enableinterrupts
		I2C2->CR2 |= I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN;
	}

	if (tmp & I2C_CR1_STOP){
		return xpcc::i2c::BUSY;
	}
	else{
		return busyState;
	}
}

xpcc::i2c::BusState
xpcc::stm32::I2c2::getBusState()
{
	return busState;
}
