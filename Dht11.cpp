#include "Dht11.h"
#include "interrupts.h"

Dht11::Dht11()
{
	_maxcycles = microsecondsToClockCycles(1000);  // 1 millisecond timeout for
												   // reading pulses from DHT sensor.
	// Note that count is now ignored as the DHT reading algorithm adjusts itself
	// basd on the speed of the processor.

	begin();
}

DeviceErrorCode Dht11::read(String& data)
{
	// Print a friendly reading direct to the user.
	data = "Error reading";
	int count = 0;
	while (!_read())
	{
		count++;
		delay(100);
		if (count > 10)
		{
			return ERROR_READ;
		}
	}

	auto temp = readTemperature();
	auto humidity = readHumidity();
	data = String(temp);
	return DEVICE_OK;
}

DeviceErrorCode Dht11::readAll(String& data)
{
	// Print a friendly reading direct to the user.
	data = "Error reading";
	int count = 0;
	while(!_read())
	{
		count++;
		delay(100);
		if (count > 10)
		{
			return ERROR_READ;
		}
	}

	auto temp = readTemperature();
	auto humidity = readHumidity();
	data = String("Temp: ") + String(temp) + String("  Humidity: ") + String(humidity);
	return DEVICE_OK;
}

void Dht11::begin()
{
	pinMode(PIN, INPUT_PULLUP);
	// Using this value makes sure that millis() - lastreadtime will be
	// >= MIN_INTERVAL right away. Note that this assignment wraps around,
	// but so will the subtraction.
	_lastreadtime = -MIN_INTERVAL;
	DEBUG_PRINT("Max clock cycles: "); DEBUG_PRINTLN(_maxcycles, DEC);
}


float Dht11::readTemperature()
{
	return convertCtoF(data[2]);
}

float Dht11::readHumidity()
{
	return data[0];
}

float Dht11::convertCtoF(const float& c)
{
	return c * 1.8 + 32;
}

float Dht11::convertFtoC(const float& f)
{
	return (f - 32) * 0.55555;
}

float Dht11::computeHeatIndex(const float& temperature, const float& percentHumidity)
{
	// Using both Rothfusz and Steadman's equations
	// http://www.wpc.ncep.noaa.gov/html/heatindex_equation.shtml
	float heatIndex;
	heatIndex = 0.5 * (temperature + 61.0 + ((temperature - 68.0) * 1.2) + (percentHumidity * 0.094));

	if (heatIndex > 79)
	{
		heatIndex = -42.379 +
			2.04901523 * temperature +
			10.14333127 * percentHumidity +
			-0.22475541 * temperature*percentHumidity +
			-0.00683783 * pow(temperature, 2) +
			-0.05481717 * pow(percentHumidity, 2) +
			0.00122874 * pow(temperature, 2) * percentHumidity +
			0.00085282 * temperature*pow(percentHumidity, 2) +
			-0.00000199 * pow(temperature, 2) * pow(percentHumidity, 2);

		if ((percentHumidity < 13) && (temperature >= 80.0) && (temperature <= 112.0))
		{
			heatIndex -= ((13.0 - percentHumidity) * 0.25) * sqrt((17.0 - abs(temperature - 95.0)) * 0.05882);
		}

		else if ((percentHumidity > 85.0) && (temperature >= 80.0) && (temperature <= 87.0))
		{
			heatIndex += ((percentHumidity - 85.0) * 0.1) * ((87.0 - temperature) * 0.2);
		}
	}

	return heatIndex;
}

bool Dht11::_read()
{
	// Check if sensor was read less than two seconds ago and return early
	// to use last reading.
	uint32_t currenttime = millis();
	if ((currenttime - _lastreadtime) < 2000)
	{
		return _lastresult; // return last correct measurement
	}
	_lastreadtime = currenttime;

	// Reset 40 bits of received data to zero.
	data[0] = data[1] = data[2] = data[3] = data[4] = 0;

	// Send start signal.  See DHT datasheet for full signal diagram:
	//   http://www.adafruit.com/datasheets/Digital%20humidity%20and%20temperature%20sensor%20AM2302.pdf

	// Go into high impedence state to let pull-up raise data line level and
	// start the reading process.
	digitalWrite(PIN, HIGH);
	delay(250);

	// First set data line low for 20 milliseconds.
	pinMode(PIN, OUTPUT);
	digitalWrite(PIN, LOW);
	delay(20);

	uint32_t cycles[80];
	{
		// Turn off interrupts temporarily because the next sections are timing critical
		// and we don't want any interruptions.
		InterruptLock lock;

		// End the start signal by setting data line high for 40 microseconds.
		digitalWrite(PIN, HIGH);
		delayMicroseconds(40);

		// Now start reading the data line to get the value from the DHT sensor.
		pinMode(PIN, INPUT_PULLUP);
		delayMicroseconds(10);  // Delay a bit to let sensor pull data line low.

		// First expect a low signal for ~80 microseconds followed by a high signal
		// for ~80 microseconds again.
		if (expectPulse(LOW) == 0)
		{
			DEBUG_PRINTLN(F("Timeout waiting for start signal low pulse."));
			_lastresult = false;
			return _lastresult;
		}

		if (expectPulse(HIGH) == 0)
		{
			DEBUG_PRINTLN(F("Timeout waiting for start signal high pulse."));
			_lastresult = false;
			return _lastresult;
		}

		// Now read the 40 bits sent by the sensor.  Each bit is sent as a 50
		// microsecond low pulse followed by a variable length high pulse.  If the
		// high pulse is ~28 microseconds then it's a 0 and if it's ~70 microseconds
		// then it's a 1.  We measure the cycle count of the initial 50us low pulse
		// and use that to compare to the cycle count of the high pulse to determine
		// if the bit is a 0 (high state cycle count < low state cycle count), or a
		// 1 (high state cycle count > low state cycle count). Note that for speed all
		// the pulses are read into a array and then examined in a later step.
		for (int i = 0; i < 80; i += 2)
		{
			cycles[i] = expectPulse(LOW);
			cycles[i + 1] = expectPulse(HIGH);
		}
	} // Timing critical code is now complete.

	// Inspect pulses and determine which ones are 0 (high state cycle count < low
	// state cycle count), or 1 (high state cycle count > low state cycle count).
	for (int i = 0; i < 40; ++i)
	{
		uint32_t lowCycles = cycles[2 * i];
		uint32_t highCycles = cycles[2 * i + 1];
		if ((lowCycles == 0) || (highCycles == 0))
		{
			DEBUG_PRINTLN(F("Timeout waiting for pulse."));
			_lastresult = false;
			return _lastresult;
		}
		data[i / 8] <<= 1;
		// Now compare the low and high cycle times to see if the bit is a 0 or 1.
		if (highCycles > lowCycles)
		{
			// High cycles are greater than 50us low cycle count, must be a 1.
			data[i / 8] |= 1;
		}
		// Else high cycles are less than (or equal to, a weird case) the 50us low
		// cycle count so this must be a zero.  Nothing needs to be changed in the
		// stored data.
	}

	DEBUG_PRINTLN(F("Received:"));
	DEBUG_PRINT(data[0], HEX); DEBUG_PRINT(F(", "));
	DEBUG_PRINT(data[1], HEX); DEBUG_PRINT(F(", "));
	DEBUG_PRINT(data[2], HEX); DEBUG_PRINT(F(", "));
	DEBUG_PRINT(data[3], HEX); DEBUG_PRINT(F(", "));
	DEBUG_PRINT(data[4], HEX); DEBUG_PRINT(F(" =? "));
	DEBUG_PRINTLN((data[0] + data[1] + data[2] + data[3]) & 0xFF, HEX);

	// Check we read 40 bits and that the checksum matches.
	if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF))
	{
		_lastresult = true;
		return _lastresult;
	}
	else
	{
		DEBUG_PRINTLN(F("Checksum failure!"));
		_lastresult = false;
		return _lastresult;
	}
}

uint32_t Dht11::expectPulse(bool level)
{
	// Expect the signal line to be at the specified level for a period of time and
	// return a count of loop cycles spent at that level (this cycle count can be
	// used to compare the relative time of two pulses).  If more than a millisecond
	// ellapses without the level changing then the call fails with a 0 response.
	// This is adapted from Arduino's pulseInLong function (which is only available
	// in the very latest IDE versions):
	//   https://github.com/arduino/Arduino/blob/master/hardware/arduino/avr/cores/arduino/wiring_pulse.c
	uint32_t count = 0;
	while (digitalRead(PIN) == level)
	{
		if (count++ >= _maxcycles)
		{
			return 0; // Exceeded timeout, fail.
		}
	}

	return count;
}

void Dht11::createEmailMessage(String& subject, String& message, DeviceMessageType deviceMessageType)
{
	subject = String(Configuration::Instance()->deviceName);
	if (deviceMessageType == MAX_RANGE)
	{
		subject += String(" MAX Temp Warning");
		message = String(Configuration::Instance()->deviceName) + String("  has triggered the maximum temperature range.");
	}
	else if (deviceMessageType == MIN_RANGE)
	{
		subject += String(" MIN Temp Warning");
		message = String(Configuration::Instance()->deviceName) + String("  has triggered the minimum temperature range.");
	}
	else if (deviceMessageType == OFF_LINE)
	{
		subject += String(" Offline Warning");
		message = String(Configuration::Instance()->deviceName) + String(" is now offline.");
	}
	else if (deviceMessageType == ON_LINE)
	{
		subject += String(" Online");
		message = String(Configuration::Instance()->deviceName) + String(" is now online.");
	}
	else if (deviceMessageType == DISCONNECTED)
	{
		subject += String(" Disconnected Warning");
		message = String(Configuration::Instance()->deviceName) + String(" sensor is disconnected.");
	}
}

String Dht11::getName()
{
	return "Dht11";
}

String Dht11::indexComponent()
{
	return "";
}