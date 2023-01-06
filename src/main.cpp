#include <ESP8266WiFi.h>
#include <stdio.h>
#include <ArduinoJson.h>
#include "WiFiManager.h"
#include "Ticker.h"
#include "PubSubClient.h"

// Fix SPIFFS being deprecated warning for now.
#include <FS.h>
#define SPIFFS LittleFS     // Lazy solution. Need real fix later.
#include <LittleFS.h> 

#include "settings.h"

// Status LEDs
#define SOLAR_LED 	14
#define REQUEST_LED     12
#define WIFI_SWITCH 	5
#define PWM_OUTPUT      4

// NodeName moet verschillend zijn voor elke MQTT client. Opgepast.
// Omdat er al een smartmeter actief is, tijdelijk een andere naam gekozen voor het MQTT topic
// #define NodeName "SmartMeter"
#define NodeName "SmartMeter_2023"
#define Base_Topic  NodeName "/"
#define Data_Topic  Base_Topic "P1Data"
#define will_Topic  Base_Topic "LWT"
#define will_QoS 0
#define will_Retain true
#define will_Message "offline"

#define version_Topic  Base_Topic "version"
#define Gateway_AnnouncementMsg "online"

//------------------------------------------------------------------------------------
// Prototypes
//------------------------------------------------------------------------------------
void TickerUpdate();
void setRequestLedOn(void);
void setRequestLedOff(void);
void processLine(int len);
void setSolarLed(unsigned char Status);

//------------------------------------------------------------------------------------
// Timers
//------------------------------------------------------------------------------------
// Create 5 second main timer.
// Infinite repeat
Ticker timer1(TickerUpdate, 5000, 0);

// Create 500 msec timer for "request data signal"
// 1 Repeat
Ticker timer2(setRequestLedOff, 500, 1);

WiFiClient espClient;
PubSubClient client(espClient);

int MQTTCounter;
String AP_NameString;

//------------------------------------------------------------------------------------
// Slimme meter specifieke code
// https://github.com/daniel-jong/esp8266_p1meter/blob/master/esp8266_p1meter/esp8266_p1meter.ino
//------------------------------------------------------------------------------------
// * Send a message to a broker topic
void send_mqtt_message(const char *topic, char *payload)
{
    Serial.printf("MQTT Outgoing on %s: ", topic);
    Serial.println(payload);

    bool result = client.publish(topic, payload, false);

    if (!result)
    {
        Serial.printf("MQTT publish to topic %s failed\n", topic);
    }
}

void send_metric(String name, long metric)
{
    char output[10];
    ltoa(metric, output, sizeof(output));

    String topic = String(Data_Topic) + "/" + name;
    send_mqtt_message(topic.c_str(), output);
}

void send_data_to_broker()
{
    send_metric("consumption_low_tarif", CONSUMPTION_LOW_TARIF);
    send_metric("consumption_high_tarif", CONSUMPTION_HIGH_TARIF);
    send_metric("returndelivery_low_tarif", RETURNDELIVERY_LOW_TARIF);
    send_metric("returndelivery_high_tarif", RETURNDELIVERY_HIGH_TARIF);
    send_metric("actual_consumption", ACTUAL_CONSUMPTION);
    send_metric("actual_returndelivery", -1.0 * ACTUAL_RETURNDELIVERY);

//    send_metric("l1_instant_power_usage", L1_INSTANT_POWER_USAGE);
//    send_metric("l2_instant_power_usage", L2_INSTANT_POWER_USAGE);
//    send_metric("l3_instant_power_usage", L3_INSTANT_POWER_USAGE);
//    send_metric("l1_instant_power_current", L1_INSTANT_POWER_CURRENT);
//    send_metric("l2_instant_power_current", L2_INSTANT_POWER_CURRENT);
//    send_metric("l3_instant_power_current", L3_INSTANT_POWER_CURRENT);
//    send_metric("l1_voltage", L1_VOLTAGE);
//    send_metric("l2_voltage", L2_VOLTAGE);
//    send_metric("l3_voltage", L3_VOLTAGE);
    
    send_metric("gas_meter_m3", GAS_METER_M3);

//    send_metric("actual_tarif_group", ACTUAL_TARIF);
//    send_metric("short_power_outages", SHORT_POWER_OUTAGES);
//    send_metric("long_power_outages", LONG_POWER_OUTAGES);
//    send_metric("short_power_drops", SHORT_POWER_DROPS);
//    send_metric("short_power_peaks", SHORT_POWER_PEAKS);
}

unsigned int CRC16(unsigned int crc, unsigned char *buf, int len)
{
    for (int pos = 0; pos < len; pos++)
    {
        crc ^= (unsigned int)buf[pos];    // * XOR byte into least sig. byte of crc
                                          // * Loop over each bit
        for (int i = 8; i != 0; i--)
        {
            // * If the LSB is set
            if ((crc & 0x0001) != 0)
            {
                // * Shift right and XOR 0xA001
                crc >>= 1;
                crc ^= 0xA001;
            }
            // * Else LSB is not set
            else
                // * Just shift right
                crc >>= 1;
        }
    }
    return crc;
}

bool isNumber(char *res, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (((res[i] < '0') || (res[i] > '9')) && (res[i] != '.' && res[i] != 0))
            return false;
    }
    return true;
}

int FindCharInArrayRev(char array[], char c, int len)
{
    for (int i = len - 1; i >= 0; i--)
    {
        if (array[i] == c)
            return i;
    }
    return -1;
}
long getValue(char *buffer, int maxlen, char startchar, char endchar)
{
    int s = FindCharInArrayRev(buffer, startchar, maxlen - 2);
    int l = FindCharInArrayRev(buffer, endchar, maxlen - 2) - s - 1;

    char res[16];
    memset(res, 0, sizeof(res));

    if (strncpy(res, buffer + s + 1, l))
    {
        if (endchar == '*')
        {
            if (isNumber(res, l))
                // * Lazy convert float to long
                return (1000 * atof(res));
        }
        else if (endchar == ')')
        {
            if (isNumber(res, l))
                return atof(res);
        }
    }
    return 0;
}

bool decode_telegram(int len)
{
    int startChar = FindCharInArrayRev(telegram, '/', len);
    int endChar = FindCharInArrayRev(telegram, '!', len);
    bool validCRCFound = false;

    for (int cnt = 0; cnt < len; cnt++) 
    {
        Serial.print(telegram[cnt]);
    }
    Serial.print("\n");

    if (startChar >= 0)
    {
        // * Start found. Reset CRC calculation
        currentCRC = CRC16(0x0000,(unsigned char *) telegram+startChar, len-startChar);
    }
    else if (endChar >= 0)
    {
        // * Add to crc calc
        currentCRC = CRC16(currentCRC,(unsigned char*)telegram+endChar, 1);

        char messageCRC[5];
        strncpy(messageCRC, telegram + endChar + 1, 4);

        messageCRC[4] = 0;
        validCRCFound = (strtol(messageCRC, NULL, 16) == (long int) currentCRC);

        if (validCRCFound)
            Serial.println(F("CRC Valid!"));
        else
            Serial.println(F("CRC Invalid!"));

        currentCRC = 0;
    }
    else
    {
        currentCRC = CRC16(currentCRC, (unsigned char*) telegram, len);
    }

    // 1-0:1.8.1(000992.992*kWh)
    // 1-0:1.8.1 = Elektra verbruik laag tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.8.1", strlen("1-0:1.8.1")) == 0)
    {
        CONSUMPTION_LOW_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:1.8.2(000560.157*kWh)
    // 1-0:1.8.2 = Elektra verbruik hoog tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.8.2", strlen("1-0:1.8.2")) == 0)
    {
        CONSUMPTION_HIGH_TARIF = getValue(telegram, len, '(', '*');
    }
    
    // 1-0:2.8.1(000560.157*kWh)
    // 1-0:2.8.1 = Elektra teruglevering laag tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:2.8.1", strlen("1-0:2.8.1")) == 0)
    {
        RETURNDELIVERY_LOW_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:2.8.2(000560.157*kWh)
    // 1-0:2.8.2 = Elektra teruglevering hoog tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:2.8.2", strlen("1-0:2.8.2")) == 0)
    {
        RETURNDELIVERY_HIGH_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:1.7.0(00.424*kW) Actueel verbruik
    // 1-0:1.7.x = Electricity consumption actual usage (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.7.0", strlen("1-0:1.7.0")) == 0)
    {
        ACTUAL_CONSUMPTION = getValue(telegram, len, '(', '*');
    }

    // 1-0:2.7.0(00.000*kW) Actuele teruglevering (-P) in 1 Watt resolution
    if (strncmp(telegram, "1-0:2.7.0", strlen("1-0:2.7.0")) == 0)
    {
        ACTUAL_RETURNDELIVERY = getValue(telegram, len, '(', '*');
    }

    // 1-0:21.7.0(00.378*kW)
    // 1-0:21.7.0 = Instantaan vermogen Elektriciteit levering L1
    if (strncmp(telegram, "1-0:21.7.0", strlen("1-0:21.7.0")) == 0)
    {
        L1_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:41.7.0(00.378*kW)
    // 1-0:41.7.0 = Instantaan vermogen Elektriciteit levering L2
    if (strncmp(telegram, "1-0:41.7.0", strlen("1-0:41.7.0")) == 0)
    {
        L2_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:61.7.0(00.378*kW)
    // 1-0:61.7.0 = Instantaan vermogen Elektriciteit levering L3
    if (strncmp(telegram, "1-0:61.7.0", strlen("1-0:61.7.0")) == 0)
    {
        L3_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:31.7.0(002*A)
    // 1-0:31.7.0 = Instantane stroom Elektriciteit L1
    if (strncmp(telegram, "1-0:31.7.0", strlen("1-0:31.7.0")) == 0)
    {
        L1_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }
    // 1-0:51.7.0(002*A)
    // 1-0:51.7.0 = Instantane stroom Elektriciteit L2
    if (strncmp(telegram, "1-0:51.7.0", strlen("1-0:51.7.0")) == 0)
    {
        L2_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }
    // 1-0:71.7.0(002*A)
    // 1-0:71.7.0 = Instantane stroom Elektriciteit L3
    if (strncmp(telegram, "1-0:71.7.0", strlen("1-0:71.7.0")) == 0)
    {
        L3_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }

    // 1-0:32.7.0(232.0*V)
    // 1-0:32.7.0 = Voltage L1
    if (strncmp(telegram, "1-0:32.7.0", strlen("1-0:32.7.0")) == 0)
    {
        L1_VOLTAGE = getValue(telegram, len, '(', '*');
    }
    // 1-0:52.7.0(232.0*V)
    // 1-0:52.7.0 = Voltage L2
    if (strncmp(telegram, "1-0:52.7.0", strlen("1-0:52.7.0")) == 0)
    {
        L2_VOLTAGE = getValue(telegram, len, '(', '*');
    }   
    // 1-0:72.7.0(232.0*V)
    // 1-0:72.7.0 = Voltage L3
    if (strncmp(telegram, "1-0:72.7.0", strlen("1-0:72.7.0")) == 0)
    {
        L3_VOLTAGE = getValue(telegram, len, '(', '*');
    }

    // 0-1:24.2.1(150531200000S)(00811.923*m3)
    // 0-1:24.2.1 = Gas (DSMR v4.0) on Kaifa MA105 meter
    if (strncmp(telegram, "0-1:24.2.1", strlen("0-1:24.2.1")) == 0)
    {
        GAS_METER_M3 = getValue(telegram, len, '(', '*');
    }

    // 0-0:96.14.0(0001)
    // 0-0:96.14.0 = Actual Tarif
    if (strncmp(telegram, "0-0:96.14.0", strlen("0-0:96.14.0")) == 0)
    {
        ACTUAL_TARIF = getValue(telegram, len, '(', ')');
    }

    // 0-0:96.7.21(00003)
    // 0-0:96.7.21 = Aantal onderbrekingen Elektriciteit
    if (strncmp(telegram, "0-0:96.7.21", strlen("0-0:96.7.21")) == 0)
    {
        SHORT_POWER_OUTAGES = getValue(telegram, len, '(', ')');
    }

    // 0-0:96.7.9(00001)
    // 0-0:96.7.9 = Aantal lange onderbrekingen Elektriciteit
    if (strncmp(telegram, "0-0:96.7.9", strlen("0-0:96.7.9")) == 0)
    {
        LONG_POWER_OUTAGES = getValue(telegram, len, '(', ')');
    }

    // 1-0:32.32.0(00000)
    // 1-0:32.32.0 = Aantal korte spanningsdalingen Elektriciteit in fase 1
    if (strncmp(telegram, "1-0:32.32.0", strlen("1-0:32.32.0")) == 0)
    {
        SHORT_POWER_DROPS = getValue(telegram, len, '(', ')');
    }

    // 1-0:32.36.0(00000)
    // 1-0:32.36.0 = Aantal korte spanningsstijgingen Elektriciteit in fase 1
    if (strncmp(telegram, "1-0:32.36.0", strlen("1-0:32.36.0")) == 0)
    {
        SHORT_POWER_PEAKS = getValue(telegram, len, '(', ')');
    }

    return validCRCFound;
}

void read_p1_hardwareserial()
{
    if (Serial.available())
    {
        memset(telegram, 0, sizeof(telegram));

        while (Serial.available())
        {
            ESP.wdtDisable();
            int len = Serial.readBytesUntil('\n', telegram, P1_MAXLINELENGTH);
            ESP.wdtEnable(1);

            processLine(len);
        }
    }
}

void processLine(int len) 
{
    telegram[len] = '\n';
    telegram[len + 1] = 0;
    yield();

    bool result = decode_telegram(len + 1);
    if (result) 
    {
        // Set analog output
        if (ACTUAL_RETURNDELIVERY > 0)
        {
            analogWrite(PWM_OUTPUT, (2 * (ACTUAL_RETURNDELIVERY)) / 5);
            setSolarLed(1);
        }
        else
        {
            analogWrite(PWM_OUTPUT, (2 * (ACTUAL_CONSUMPTION)) / 5);
            setSolarLed(0);
        }

        if (MQTTCounter == 0)
        {
            send_data_to_broker();
            LAST_UPDATE_SENT = millis();
        }

        // Beperk het aantal berichten naar de MQTT server tot 1 per 30 second (6 * 5 sec update rate)
        MQTTCounter += 1;
        if (MQTTCounter >= 6)
        {
            MQTTCounter = 0;
        }
    }
}

//------------------------------------------------------------------------------------
//define your default values here, if there are different values in config.json, they are overwritten.
//------------------------------------------------------------------------------------
char configfilename[40] = "/config.txt";
char mqtt_server[40] 	= "192.168.3.130";
char mqtt_port[20] 		= "1883";
char mqtt_user[20]    	= "mqtt";
char mqtt_password[20]	= "welkom";

WiFiManager wifiManager;

WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 20);
WiFiManagerParameter custom_mqtt_user("user", "mqtt user", mqtt_user, 20);
WiFiManagerParameter custom_mqtt_password("password", "mqtt password", mqtt_password, 20);

//------------------------------------------------------------------------------------
char msg[100];
unsigned char ticker_counter;
unsigned int  char_counter;

unsigned char led_counter = 0;

// UART receive buffer
#define MAXBUFFER 		250
char buffer[MAXBUFFER+1];
int incomingByte = 0; // for incoming serial data

//------------------------------------------------------------------------------------
//flag for saving data
//------------------------------------------------------------------------------------
bool shouldSaveConfig = false;

//------------------------------------------------------------------------------------
//callback notifying us of the need to save config
//------------------------------------------------------------------------------------
void saveConfigCallback ()
{
	Serial.println("Should save config");
	shouldSaveConfig = true;
}

void Setup_Wifi()
{
	WiFi.enableAP(false);
	WiFi.enableSTA(true);
	WiFi.begin();

	// while wifi not connected yet, print '.'
	// then after it connected, get out of the loop
	while (WiFi.status() != WL_CONNECTED)
	{
		delay(500);
		Serial.print(".");
	}

	//print a new line, then print WiFi connected and the IP address
	Serial.println("");
	Serial.println("WiFi connected.");

	// Print the IP address
	Serial.println(WiFi.localIP());
}

void Connect_MQTT()
{
	if (client.connect(NodeName,  mqtt_user, mqtt_password, will_Topic, will_QoS, will_Retain, will_Message))
	{
		Serial.println("MQTT connected.");

		// Publish LWT "online"
		client.publish(will_Topic, Gateway_AnnouncementMsg, will_Retain);
		client.publish(version_Topic,  "Compiled: " __DATE__ ", " __TIME__, false);
	}
	else
	{
		Serial.println("MQTT not connected.");
	}
}

//------------------------------------------------------------------------------------
// TickerUpdate callback
//------------------------------------------------------------------------------------
void TickerUpdate()
{
	Serial.println("Tick!");
	ticker_counter++;

	// Check wifi status
	if (WiFi.status() != WL_CONNECTED )
	{
        Serial.print("WiFi not connected. Trying to setup wifi");
		Setup_Wifi();

		// Try to re-connect MQTT
		if (WiFi.status() == WL_CONNECTED )
		{
			Connect_MQTT();
		}
	}
	else
	{
		if (client.state() == MQTT_CONNECTED)
		{
            // Request data from meter
            Serial.println("Request");
            setRequestLedOn();
		}
		else
		{
			Serial.print("MQTT connection lost. Trying to restore connection. ");
			Connect_MQTT();
		}
	}
}

void callback(char* topic, byte* payload, unsigned int length) 
{
}

void setSolarLed(unsigned char Status)
{
	digitalWrite(SOLAR_LED, Status);
}

void setRequestLedOn(void)
{
    digitalWrite(REQUEST_LED, 1);

    // Keep data reqeust line high for 500 msec.
    timer2.start();
}

void setRequestLedOff(void)
{
    digitalWrite(REQUEST_LED, 0);
}

void WriteConfigFile()
{
    Serial.println("-------------------------------------------------------");
    Serial.println("WriteConfigFile()");
	Serial.println(mqtt_server);
	Serial.println(mqtt_port);
	Serial.println(mqtt_user);
	Serial.println(mqtt_password);

	if (SPIFFS.begin())
	{
		File configFile = SPIFFS.open(configfilename, "w");

		// Converteer de char array naar string om zo println te kunnen gebruiken
		String text;

		text = String(mqtt_server);
		configFile.println(text);

		text = String(mqtt_port);
		configFile.println(text);

		text = String(mqtt_user);
		configFile.println(text);

		text = String(mqtt_password);
		configFile.println(text);

		configFile.close();
	}

    Serial.println("-------------------------------------------------------");
}

void ReadConfigFile()
{
    Serial.println("+++++++++++++++++++++++++++++++++++++++++++++++++++++++");
	Serial.println("ReadConfigFile()");
	String line;

	if (SPIFFS.begin())
	{
		Serial.println("mounted file system");

		if (SPIFFS.exists(configfilename))
		{
			// file exists, reading and loading
			Serial.println("reading config file");
			File configFile = SPIFFS.open(configfilename, "r");

			if (configFile)
			{
				line = configFile.readStringUntil('\n');
				line[line.length()-1] = '\0';
				line.toCharArray(mqtt_server, line.length());
				Serial.println(mqtt_server);

				line = configFile.readStringUntil('\n');
				line[line.length()-1] = '\0';
				line.toCharArray(mqtt_port, line.length());
				Serial.println(mqtt_port);

				line = configFile.readStringUntil('\n');
				line[line.length()-1] = '\0';
				line.toCharArray(mqtt_user, line.length());
				Serial.println(mqtt_user);

				line = configFile.readStringUntil('\n');
				line[line.length()-1] = '\0';
				line.toCharArray(mqtt_password, line.length());
				Serial.println(mqtt_password);

			    configFile.close();
			}
		}
	}
	else
	{
		Serial.println("Failed to mount FS");
	}

    Serial.println("+++++++++++++++++++++++++++++++++++++++++++++++++++++++");
}

//------------------------------------------
void setup() 
{
    // Init variables
    MQTTCounter = 0;
    
    // Wait for stable power
	delay(1000);

	pinMode(SOLAR_LED, OUTPUT);
    pinMode(REQUEST_LED,   OUTPUT);
	pinMode(WIFI_SWITCH, INPUT);

    setSolarLed(1);

    //-------------------------------------------------------------------
    // MFH
    // GEN 1 smart meter = 9600
    // All other GEN's = 115200
    //-------------------------------------------------------------------
    //Setup Serial port speed
    Serial.begin(115200);

    //-------------------------------------------------------------------
    // MFH
    // P1 output is inverted. Use either a hardware inverter, or invert the RX pin for the UART.
    // Invert the RX serialport by setting a register value, this way the TX might continue normally allowing the serial monitor to read println's
    //-------------------------------------------------------------------
    Serial.println("Inverting RX pin & swapping UART!");
    USC0(UART0) = USC0(UART0) | BIT(UCRXI);

    //-------------------------------------------------------------------
    // MFH
    // On a NODEMCU, swap of SERIAL pins is needed as the USB-Serial chip is hardwired to RXD0 & TXD0.
    // Therefore the smartmeter P1 cannot be connected to the RXD0.
    // Serial.swap() will change the SERIAL pins to RXD2 & TXD2.
    // The UART remains UART0, just pins are re-mapped.
    // Another call Serial.swap() will change them back.
    // Connect a seperate USB-SERIAL converter to TXD2 to monitor debug logging if needed.
    //-------------------------------------------------------------------
    delay(10);
    Serial.swap();
    delay(10);

    Serial.println("Serial port is ready to recieve.");

    //-------------------------------------------------------------------
    // Read config file
    //-------------------------------------------------------------------
    ReadConfigFile();

    //-------------------------------------------------------------------
    // Create host name with MAC address to make client unique
    // Do a little work to get a unique-ish name. Append the
    // last two bytes of the MAC (HEX'd) to the NodeName
    //-------------------------------------------------------------------
    // uint8_t mac[WL_MAC_ADDR_LENGTH];
    // WiFi.softAPmacAddress(mac);
    // String macID = String(mac[WL_MAC_ADDR_LENGTH - 2], HEX) + String(mac[WL_MAC_ADDR_LENGTH - 1], HEX);
    // macID.toUpperCase();
    // String AP_NameString = NodeName + macID;

	//-------------------------------------------------------------------
    // Wifi manager
	//-------------------------------------------------------------------
	wifiManager.addParameter(&custom_mqtt_server);
	wifiManager.addParameter(&custom_mqtt_port);
	wifiManager.addParameter(&custom_mqtt_user);
	wifiManager.addParameter(&custom_mqtt_password);

	//set config save notify callback
	wifiManager.setSaveConfigCallback(saveConfigCallback);

    //------------------------------------------------------------------------------------
    // Connect the "WIFI_SWITCH" pin to V+ to force the wifiManager to show up.
    // This can be useful if you want to change the wifi network the ESP is connected too.
    // wifiManager.autoConnect() will connect if the SSID/PASS are valid, so no way to connect 
    // it to a different WIFI, unless we force the ConfigPortal to load.
    //------------------------------------------------------------------------------------
	if (digitalRead(WIFI_SWITCH) == 1)
	{
	    wifiManager.startConfigPortal(NodeName);
	}
	else
	{
		 wifiManager.autoConnect(NodeName);
	}

	// Alleen de nieuwe parameters overnemen als er op "Save" gedrukt is.
	// Anders zijn deze velden leeg.
    if (shouldSaveConfig)
    {
		strcpy(mqtt_server, custom_mqtt_server.getValue());
		strcpy(mqtt_port, custom_mqtt_port.getValue());
		strcpy(mqtt_user, custom_mqtt_user.getValue());
		strcpy(mqtt_password, custom_mqtt_password.getValue());

    	WriteConfigFile();
    }

    // Start wifi
    Setup_Wifi();

    // Configure MQTT
    client.setServer(mqtt_server, atoi(mqtt_port));
    client.setCallback(callback);

    // Connect MQTT
    Connect_MQTT();

    //ts.add(0, 10000, [&](void*) { TickerUpdate(); }, nullptr, true);

    // Start timer update
    timer1.start();
}

void loop()
{
	// Timers update
	timer1.update();
	timer2.update();

	// Update MQTT to keep connection alive.
	client.loop();

    // Read any data from the slimme meter on RXD2
    read_p1_hardwareserial();
}
