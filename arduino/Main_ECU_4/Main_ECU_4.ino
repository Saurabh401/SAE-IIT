#include <IFCT.h>                 // ImprovedFLexCanLibrary
typedef uint32_t uint32;          // CAN_message_t id type
typedef void (*flagReader)(void); // functions that are called when flag bits are true

// TODO: decide on addresses for all the sensors and bms
enum CanADR : uint32 {
    // motor
    TEMP1 = 0x0A0,
    TEMP2 = 0x0A1,
    TEMP3 = 0x0A2,
    ANLIV = 0x0A3,
    DIGIS = 0x0A4,
    MOTOR_POS = 0x0A5,
    CURRENT = 0x0A6,
    VOLTAGE = 0x0A7,
    FAULT = 0x0AB,
    // Yes
    FOO = 0x0FF,
    BAR = 0x0BB,
};

//TODO: make motorPackets work with TTMsg
struct motorDataPkt {
    uint32 idOffset = 0;
    int values[8][8]; // not all the tables will be used eg. ANLIV & DIGIS
    bool faults[8][8];
} motor0, motor1;

// Used to identify what sensors go into what message
enum sensor { // Pin0 must be sacrificed to the gods to have a nil value
    NIL = 0,
    fooSenPin = 5,
    barSenPin = 19,
    startBPin = 23,
};

// TODO: figure out how data will be pushed to andriod // andriod will decode bytes based off address
/*
    Byte # (map)| 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 | // byte index in CAN message
    H&L 'packet'        | H | L |   =   |  PKT  | // L and H bytes must be next to each other, referred as packets(PKT)
    PKT pos     |  PKT  |  PKT  |  PKT  |  PKT  | // Valid PKT placement NOTE: is set as seprate H & L in actual buffer
    sensor      |   | S3|   | S2|   | S1|   | S0| // Where sensors are called (based off pos in TTMsg table | Byte#/2)
    FLAG pos    | F | F | F | F | F | F | F | F | // Valid FLAG placement NOTE: currently only one flag per msg
*/
typedef struct TTMsg {       // Teensy to Teensy message definition/structure
    uint32 address;          // identifies how the msg should be interpreted using it's address
    sensor sensors[4];       // sensors that have data in this message; position in table sets where PKT goes (see ^)
    char flagPos = -1;       // where the flag goes as shown above ^ NOTE: negative means no flag
    flagReader flagFuncs[8]; // functions that are called when flag bit is true; limits each packet to one flag byte
    byte flag;               // stored flag value
    byte values[8];          // values from bytes that can be pushed after reading
    // Maybe add single packet interpreter for special cases such as motor? default to normal pkt reading?
} TTMsg;

/* ----- ECU specific data ----- */

// more logical examples
void preCharge() {
    Serial.println("NNNNNNNYYYYOOWWMMMMM!"); //called when flag bit 0 == true
}

// this message only checks the flag
TTMsg startButton{
    BAR,
    {},
    0,
    {preCharge},
};

// two packets used in this message
TTMsg fooBar{
    FOO,
    {
        //Sensor table also tells reciving ECUs what data they should be expecting
        fooSenPin,
        barSenPin,
    },
};

// load all the ECU specific messages
TTMsg TTMessages[]{
    startButton,
    fooBar,
};

/* ----- END ECU specific data ----- */

void setup() {
    // Set motorID address offsets
    motor1.idOffset = 0x30;
    motor0.idOffset = 0x0;
    for (int j = 0; j < 8; ++j) { // load all the zeros for the motor fields
        for (int k = 0; k < 8; ++k) {
            motor0.faults[j][k] = 0;
            motor1.faults[j][k] = 0;
            motor0.values[j][k] = 0;
            motor1.values[j][k] = 0;
        }
    }
    pinMode(2, OUTPUT); // Fusion Tech's Dual CAN-Bus R pin switch
    digitalWrite(2, LOW);
    Can0.setBaudRate(1000000); // Speeed
    Can0.enableFIFO();         // FirstInFirstOut
}

void loop() {
    CAN_message_t dataIn; // Can message obj
    if (Can0.read(dataIn)) {
        if (motorRead(dataIn, motor0)) { //TODO: move motor id matching to sepreate method
        } else if (motorRead(dataIn, motor1)) {
        } else {
            teensyRead(dataIn);
        }
    }
    teensyWrite();
}

int decodeByte(const byte low, const byte high) { // probably will only be used to push to andriod
    int full_data = (high * 255) + low;           // convert LH data to a value
    //TODO: is there a diffrent way to get negatives? Also what value does 128 map to ?
    return high < 128 ? full_data : map(full_data, 65280, 32640, 0, -32640); // remap for neg
}

void writeTTMsg(const TTMsg &msg) {
    // CAN value conversion : value = (highByte x 256) + lowByte
    CAN_message_t dataOut; // Can message obj
    dataOut.ext = 0;
    dataOut.id = msg.address;
    dataOut.len = 8;
    for (byte i = 0; i < 8; i += 2) {
        if (msg.flagPos == i || msg.flagPos == i + 1) { // if lowByte or highByte is a flag byte
            dataOut.buf[i] = msg.flag;                  // push stored flag byte to buf
        } else if (msg.sensors[i]) {                    // checks if data is to suppose to be here
            dataOut.buf[i] = msg.values[i];
            dataOut.buf[i + 1] = msg.values[i + 1];
        }
    }
    Can0.write(dataOut);
}

void flagScan(const byte &flag, flagReader funcTbl[8]) {
    if (flag) {                                             // check if flag has any value
        for (byte bit = 0; bit < 8; ++bit) {                // iterate though flag bits
            if (funcTbl[bit] && (flag >> bit) & 0B00000001) // check that we can do somthing if the bit is true
                funcTbl[bit]();                             // call function based off bit pos
        }
    }
}

//IMPROVE: instead of just storing values and pushing later, why not push as they are recieved? Consult leads!
void readTTMsg(TTMsg &msg, const byte buf[8]) {
    for (size_t i = 0; i < 8; i += 2) {
        // i == lowByte
        // i+1 == highByte
        if (msg.flagPos == i) {                  // allows flags to be placed anywhere
            flagScan(buf[i], msg.flagFuncs);     // iterate through lowByte bits for flags
            msg.flag = buf[i];                   // store flag value
        } else if (msg.flagPos == i + 1) {       // allows flags to be placed anywhere
            flagScan(buf[i + 1], msg.flagFuncs); // iterate through highByte bits for flags
            msg.flag = buf[i + 1];               // store flag value
        } else if (msg.sensors[i]) {             // are we expecting data on this packet?
            msg.values[i] = buf[i];              // don't encode as there is no immediate need
            msg.values[i + 1] = buf[i + 1];
        }
    }
}

// Iterate through defined TTMsgs and push their data
void teensyWrite() {
    for (TTMsg &msg : TTMessages) {
        writeTTMsg(msg);
    }
}

// Iterate though defined TTMsgs and check if the address is one of theirs
void teensyRead(const CAN_message_t &dataIn) { // IMPROVE: ensure that flag and sensor positions align up / don't overlap
    for (TTMsg &msg : TTMessages) {
        if (msg.address == dataIn.id) {
            readTTMsg(msg, dataIn.buf); // id matches; interpret data based off msg structure
            break;
        };
    }
}

// motor functions

bool motorRead(const CAN_message_t &dataIn, motorDataPkt &packet) {
    uint32 offst = packet.idOffset;
    // use map to check if id is within motor id range + motor offset
    byte pos = map(dataIn.id, TEMP1 + offst, VOLTAGE + offst, 0, 8);
    if (pos <= 7) {
        motorDecodeData(dataIn, packet.values[pos]);
        return true;
    } else if (dataIn.id == FAULT + offst) {
        motorReadFault(dataIn, packet.faults);
    }
    return false;
}

void motorDecodeData(const CAN_message_t &dataIn, int *valueTbl) {
    for (size_t i = 0; i < 8; i += 2) {
        valueTbl[i] = dataIn.buf[i];
        valueTbl[i + 1] = dataIn.buf[i + 1];
    }
}

void motorReadFault(const CAN_message_t &dataIn, bool faultTbl[8][8]) {
    for (int col = 0; col < 8; ++col) {                        // for each byte
        if (dataIn.buf[col]) {                                 // If the byte has info
            for (int row = 0; row < 8; ++row) {                // for each bit
                if (((dataIn.buf[col] >> row) & 0B00000001)) { // If each bit is true, store value
                    faultTbl[col][row] = 1;
                } else { // No fault
                    faultTbl[col][row] = 0;
                }
            }
        }
    }
}