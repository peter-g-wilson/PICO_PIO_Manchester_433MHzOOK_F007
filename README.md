# PICO_PIO_Manchester_433MHzOOK_F007
Using PICO PIO, RX decode of 433 MHz OOK Manchester encoded messages from remote F007 temperature sensors.

Example sensors are Oregon Scientific WMR86, Ambient Weather F007 and Froggit FT007.
No doubt all are manufactured/cloned in China and just re-badged.
Be aware that the encoding method may change between different versions of sensors.

For the F007, the RF receiver can be any one that does 433 MHz OOK e.g. RF Solutions AM-RX9-433P or RadioControlli RCRX-434-L
The message protocol is a bit limiting for the sofisticated receivers like HopeRF RFM65.
There are also decodes for SDR e.g.ambient_weather.c at https://github.com/merbanan/rtl_433

Its been done on Arduino’s
Rob Ward (and the people he credits), https://github.com/robwlakes/ArduinoWeatherOS decoding by delay algorithm
Ron Lewis, https://eclecticmusingsofachaoticmind.wordpress.com/2015/01/21/home-automation-temperature-sensors/ for the checksum algorithm?

Why do it?
I had a fridge that kept freezing vegetables. It was interesting to see how bad the overshoot and undershoot of the controller was.
And why not monitor the freezer too. It had a much tighter controller.
The attic was getting extra loft insulation and I wondered how cold the water tank now got.
There is a water softener in the garage that didn't want to get too cold.
So I got 5 Ambient Weather F007T sensors with base station.
The base station was rubbish, invariably displaying 'HH', impossible temperatures and push button switches from the 1960's.

The method -

Take the Raspberry Pi Manchester decode example and modify the PIO state machine to add the Arduino short and long "delay" method.
The example code expected an ideal signal that is idle low between messages and starts with a '0' as a high to low transition.
The F007 Manchester encoding is the opposite to the example. A high to low transition is a '1' and low to high is a '0'.
To make the FIFO contents easier to visualise in the debugger the FIFO's bit loading order is also reversed from the example.

The signal from the RF receiver has active gain control (AGC) and so between "real" messages the input is very noisy.
When the RF receiver begins to detect the carrier frequency from a transmitter it lowers the gain and the RF receiver signal is less noisy.
Some senders are far away and can have low battery. That doesn't help. 
The 433 MHz band is also very popular so as well as having multiple F007s asynchronously transmitting there are also other devices clashing.
The F007 sends the same message three times and, as the checksum is not perfectly robust, two messages need to be seen with the same checksum.

The method includes continuously parsing the bitstream from the state machine's FIFO to recognise the potential start of a real F007 message.
Periodically the FIFO contents, if not empty, are stored in a circular buffer, decoded in time order and put into a queue of messages.
The data bit rate is about one bit every millisecond. There are 8 words in the FIFO and each stores 32 bits.
The polling is currently set to 128 ms so there could be a 128 cycle loop within one interrupt call.
Perhaps it needs to be made shorter and triggered more frequently (32 ms perhaps) or priority adjusted. 
Periodically (using spin locks) the message queue is read, the messages formatted and displayed.
This done in the main loop, currently every 1 second.

Next steps -

Obviously, if this was the only thing the RP2040 was doing then the cost of doing it on a RP2040 is higher than, for example, an ATTiny85 (< £2).
Having offloaded the "bit-banging" part to the PIO, it only make sense to add another protocol decode on the same RP2040.
The next target will be the Fine Offset WH1080 weather station my youngest son inherited from his grandfather.
Probably also re-badged design with protocol variations over time.
This one is OOK too but not Manchester encoded. It uses 868 MHz and PWM with a "1" being a short pulse of about 500 us
 and a "0" a long pulse of about 1500 us. The gap between pulses when the carrier is off is about 1000 us.
The 868 MHz is a shared band but perhaps less noisy than 433M Hz. 
That will give outdoor temperature, humidity, wind speed/direction and rain.
I suppose I'll then have to add a BME280 for indoor temperature, humidity and barometric pressure (I2C or SPI).
And perhaps a one wire DS18B20 temperature sensor but parasitically powered for the challenge i.e. just a signal/power line and a ground.
Oh dear, only two cores. 
