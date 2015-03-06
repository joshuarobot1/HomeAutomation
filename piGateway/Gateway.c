/*
RFM69 Gateway RFM69 pushing the data to the mosquitto server
by Alexandre Bouillot

License:  CC-BY-SA, https://creativecommons.org/licenses/by-sa/2.0/
Date:  10-23-2014
File: Gateway.ino
This sketch receives RFM wireless data and forwards it to Mosquitto relay

sensorNode.sensorID,

Modifications Needed:
1)  Update encryption string "ENCRYPTKEY"
*/

/*
RFM69 Pinout:
MOSI = 11
MISO = 12
SCK = 13
SS = 8
*/

//general --------------------------------
#define SERIAL_BAUD   115200
#if 1
#define DEBUG1(expression)  fprintf(stderr, expression)
#define DEBUG2(expression, arg)  fprintf(stderr, expression, arg)
#define DEBUGLN1(expression)  
//{fprintf(stderr, expression); fprintf(stderr, "\r\n");}
#define LOG(...) do { printf(__VA_ARGS__); } while (0)
#else
#define DEBUG1(expression)
#define DEBUG2(expression, arg)
#define DEBUGLN1(expression)
#define LOG(...)
#endif

//RFM69  ----------------------------------
#include "rfm69.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#define NODEID        1    //unique for each node on same network
#define NETWORKID     101  //the same on all nodes that talk to each other
//#define FREQUENCY   RF69_433MHZ
//#define FREQUENCY   RF69_868MHZ
#define FREQUENCY     RF69_915MHZ
#define ENCRYPTKEY    "xxxxxxxxxxxxxxxx" //exactly the same 16 characters/bytes on all nodes!
#define IS_RFM69HW    //uncomment only for RFM69HW! Leave out if you have RFM69W!
#define ACK_TIME      30 // max # of ms to wait for an ack
#define RFM69_SS  8

RFM69 *rfm69;
bool promiscuousMode = false; //set to 'true' to sniff all packets on the same network
byte ackCount=0;

// Mosquitto---------------
#include <mosquitto.h>

/* How many seconds the broker should wait between sending out
 keep-alive messages. */
#define KEEPALIVE_SECONDS 60
/* Hostname and port for the MQTT broker. */
#define BROKER_HOSTNAME "localhost"
#define BROKER_PORT 1883

#define MQTT_CLIENT_ID "arduinoClient"
#define MQTT_RETRY 500

int sendMQTT = 0;

typedef struct {		
	short           nodeID; 
	short		sensorID;
	unsigned long   var1_usl; 
	float           var2_float; 
	float		var3_float;	
} 
Payload;
Payload theData;
Payload PayloadIn;

typedef struct {
	short           nodeID;
	short		sensorID;		
	unsigned long   var1_usl;
	float           var2_float;
	float		var3_float;		//
	int             var4_int;
}
SensorNode;
SensorNode sensorNode;


static void die(const char *msg);
static bool set_callbacks(struct mosquitto *m);
static bool connect(struct mosquitto *m);
static int run_loop(struct mosquitto *m);

static void MQTTSendInt(struct mosquitto* _client, int node, int sensor, int var, int val);
static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val);
static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val);

static void uso(void) {
	fprintf(stderr, "Use:\n Simply use it without args :D\n");
	exit(1);
}

int main(int argc, char* argv[]) {
	if (argc != 1) uso();
	int i;
	long packetCount = 0;

	struct mosquitto *m = mosquitto_new(MQTT_CLIENT_ID, true, null);
	if (m == NULL) { die("init() failure\n"); }

	if (!set_callbacks(m)) { die("set_callbacks() failure\n"); }
	if (!connect(m)) { die("connect() failure\n"); }

	//RFM69 ---------------------------
	rfm69 = new RFM69();
	rfm69->initialize(FREQUENCY,NODEID,NETWORKID);
	#ifdef IS_RFM69HW
	rfm69->setHighPower(); //uncomment only for RFM69HW!
	#endif
	rfm69->encrypt(ENCRYPTKEY);
	rfm69->promiscuous(promiscuousMode);
	LOG("\nListening at %d Mhz...", FREQUENCY==RF69_433MHZ ? 433 : FREQUENCY==RF69_868MHZ ? 868 : 915);
	LOG("setup complete\n");

	// subscribe to mosquitto topics (must subscribe to all new outgoing commands)
	const char* topic1 = "XXXX"; 
	mosquitto_subscribe(m, 0, topic1, 0);	

return run_loop(m);
}  // end of setup

/* Loop until it is explicitly halted or the network is lost, then clean up. */
static int run_loop(struct mosquitto *m) {
	int res;
	for (;;) {
		res = mosquitto_loop(m, 1000, 1);

		if (rfm69->receiveDone()) {
			LOG("[%d] ",rfm69->SENDERID);
			if (promiscuousMode) {
				LOG(" to [%d] ", rfm69->TARGETID);
			}

			for(int i = 0; i < rfm69->DATALEN; i++) {
				LOG("%x.", rfm69->DATA[i]); }
			LOG("\n");

			if (rfm69->DATALEN != sizeof(Payload)) {
				LOG("Invalid payload received, not matching Payload struct! %d - %d\r\n", rfm69->DATALEN, sizeof(Payload));
			}
			else {
				theData = *(Payload*)rfm69->DATA; //assume radio.DATA actually contains our struct and not something else

				//save it for i2c:
				sensorNode.nodeID = theData.nodeID;
				sensorNode.sensorID = theData.sensorID;
				sensorNode.var1_usl = theData.var1_usl;
				sensorNode.var2_float = theData.var2_float;
				sensorNode.var3_float = theData.var3_float;
				sensorNode.var4_int = rfm69->RSSI;

				LOG("Received Node ID = %d Device ID = %d Time = %d  RSSI = %d var2 = %f var3 = %f\n",
					sensorNode.nodeID,
					sensorNode.sensorID,
					sensorNode.var1_usl,
					sensorNode.var4_int,
					sensorNode.var2_float,
					sensorNode.var3_float
				);
				sendMQTT = 1;
			}

		if (rfm69->ACK_REQUESTED) {
			byte theNodeID = rfm69->SENDERID;
			rfm69->sendACK();

			// When a node requests an ACK, respond to the ACK
			// and also send a packet requesting an ACK (every 3rd one only)
			// This way both TX/RX NODE functions are tested on 1 end at the GATEWAY
			if (ackCount++%3==0) {
				//Serial.print(" Pinging node ");
				//Serial.print(theNodeID);
				//Serial.print(" - ACK...");
				//delay(3); //need this when sending right after reception .. ?
				//if (radio.sendWithRetry(theNodeID, "ACK TEST", 8, 0))  // 0 = only 1 attempt, no retries
				//  Serial.print("ok!");
				//else Serial.print("nothing");
			}
		}//end if radio.ACK_REQESTED
	} //end if radio.receive

	if (sendMQTT == 1) {
		//send var1_usl
		MQTTSendULong(m, sensorNode.nodeID, sensorNode.sensorID, 1, sensorNode.var1_usl);

		//send var2_float
		MQTTSendFloat(m, sensorNode.nodeID, sensorNode.sensorID, 2, sensorNode.var2_float);

		//send var3_float
		MQTTSendFloat(m, sensorNode.nodeID, sensorNode.sensorID, 3, sensorNode.var3_float);

		//send var4_int, RSSI
		MQTTSendInt(m, sensorNode.nodeID, sensorNode.sensorID, 4, sensorNode.var4_int);

		sendMQTT = 0;
	}//end if sendMQTT
	}


	mosquitto_destroy(m);
	(void)mosquitto_lib_cleanup();

	if (res == MOSQ_ERR_SUCCESS) {
		return 0;
	} else {
		return 1;
	}
}

	

static void MQTTSendInt(struct mosquitto * _client, int node, int sensor, int var, int val) {
	char buff_topic[6];
	char buff_message[7];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	sprintf(buff_message, "%04d%", val);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
}

static void MQTTSendULong(struct mosquitto* _client, int node, int sensor, int var, unsigned long val) {
	char buff_topic[6];
	char buff_message[12];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	sprintf(buff_message, "%u", val);
	mosquitto_publish(_client, 0, &buff_topic[0], strlen(buff_message), buff_message, 0, false);
}

static void MQTTSendFloat(struct mosquitto* _client, int node, int sensor, int var, float val) {
	char buff_topic[6];
	char buff_message[12];

	sprintf(buff_topic, "%02d%01d%01d", node, sensor, var);
	snprintf(buff_message, 12, "%f", val);

	mosquitto_publish(_client, 0, buff_topic, strlen(buff_message), buff_message, 0, false);
}



// Handing of Mosquitto messages
void callback(char* topic, byte* payload, unsigned int length) {
	// handle message arrived
	LOG("Mosquitto Callback\n");
}

/* Fail with an error message. */
static void die(const char *msg) {
	fprintf(stderr, "%s", msg);
	exit(1);
}

/* Connect to the network. */
static bool connect(struct mosquitto *m) {
	int res = mosquitto_connect(m, BROKER_HOSTNAME, BROKER_PORT, KEEPALIVE_SECONDS);
	return res == MOSQ_ERR_SUCCESS;
}

/* Callback for successful connection: add subscriptions. */
static void on_connect(struct mosquitto *m, void *udata, int res) {
	if (res == 0) {             /* success */
		LOG("Connect succeed\n");
	} else {
		die("connection refused\n");
	}
}

/* Handle a message that just arrived via one of the subscriptions. */
static void on_message(struct mosquitto *m, void *udata, const struct mosquitto_message *msg) {
	if (msg == NULL) { return; }
	LOG("-- got message @ %s: (%d, QoS %d, %s) '%s'\n",
		msg->topic, msg->payloadlen, msg->qos, msg->retain ? "R" : "!r",
		msg->payload);
		
	// create message structure
	char* tpc = msg->topic;
	unsigned long pld = *((int *)msg->payload);
	char tpcN[2];
	char tpcS[1];
	
	strncpy(tpcN, tpc, 2);
	strncpy(tpcS, &tpc[2], 1);	

	PayloadIn.nodeID = strtol(tpcN, NULL, 10);
	PayloadIn.sensorID = strtol(tpcS, NULL, 10);
	PayloadIn.var1_usl = pld-48;
	PayloadIn.var2_float = 0;
	PayloadIn.var3_float = 0;

	//LOG(" %d \n", PayloadIn.nodeID);
	//LOG(" %d \n", PayloadIn.sensorID);
	//LOG(" %d \n", PayloadIn.var1_usl);

	rfm69->send(PayloadIn.nodeID, &PayloadIn, sizeof(PayloadIn));

	LOG("-- sent message to node %d \n", PayloadIn.nodeID);

}

/* A message was successfully published. */
static void on_publish(struct mosquitto *m, void *udata, int m_id) {
	LOG("-- published successfully\n");
}

/* Successful subscription hook. */
static void on_subscribe(struct mosquitto *m, void *udata, int mid, int qos_count, const int *granted_qos) {
	LOG("-- subscribed successfully\n");
}

/* Register the callbacks that the mosquitto connection will use. */
static bool set_callbacks(struct mosquitto *m) {
	mosquitto_connect_callback_set(m, on_connect);
	mosquitto_publish_callback_set(m, on_publish);
	mosquitto_subscribe_callback_set(m, on_subscribe);
	mosquitto_message_callback_set(m, on_message);
return true;
}
