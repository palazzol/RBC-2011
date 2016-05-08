// Chronotune V2.0
// Frank Palazzolo
// Based on original TimeRadio code from Eric Merrill et al.

#include <Wire.h>
#include <EEPROM.h>
#include <RogueMP3.h>

#include "tracks.h"

DateCode track_table[TRACK_TABLE_MAX_SZ];
int track_table_sz;

// Where the dial stops
#define COUNTS_MIN   300 
#define COUNTS_MAX  2900

// Used for calibration
#define COUNTS_1850  416
#define COUNTS_2050 2786

// ATmega2560 pin connections

// Wire peripherals
#define SDIO_PIN    20
#define SCLK_PIN    21

// Reset pin on FM tuner board
#define FM_RST_PIN  13

// Tuning knob - encoder
// looks like 128 falling edges of A per revolution, plenty of resolution

#define ENCODER_A_PIN 2
#define ENCODER_B_PIN 4

// Limit switch for dial
#define MOT_LIM_PIN  5

// Drive pin for dial stepper motor
#define MOT_PIN 9
#define MOT_DIR_PIN 8  // direction of stepper

// i2c addresses

#define FM_ADDY 0x10
#define POT_1_ADDY 0x28
#define POT_2_ADDY 0x29

#define IO_1_ADDY 0x26
#define IO_2_ADDY 0x27

// encoderPos - written by ISR, read by main loop
volatile int encoderPos = 0;

int counts_1850 = COUNTS_1850;
int counts_2050 = COUNTS_2050;

void setup() {
  
  Serial.begin(57600);  // Debug
  
  Serial1.begin(4800);  // ump3 board
  
  // Rotary Encoder
  pinMode(ENCODER_A_PIN, INPUT);
  pinMode(ENCODER_B_PIN, INPUT);

  // Display
  display_init();
  displayYear(12345);
  
  // Limit switch
  pinMode(MOT_LIM_PIN, INPUT);
  digitalWrite(MOT_LIM_PIN, HIGH);  // Pull-up, active low

  // Stepper Motor Init
  pinMode(MOT_PIN, OUTPUT);
  digitalWrite(MOT_PIN, LOW);
  pinMode(MOT_DIR_PIN, OUTPUT);
  digitalWrite(MOT_DIR_PIN, LOW);

  // encoderPos is not "live" yet, reset it to the current dial position
  encoderPos = calculateStepsFromLimit();
  if (encoderPos > COUNTS_MAX) 
    encoderPos = COUNTS_MAX;
  SendDialToCountsFromLimitSwitch(encoderPos);
  
  // Encoder Interrupt Service Routine
  attachInterrupt(digitalPinToInterrupt(ENCODER_A_PIN), encoderISR, FALLING); 
  
  //FM
  fm_init();
  gotoChannel(911);
  setVolume(0, 0xaa); //FM

  // MP3
  setVolume(65, 0xa9);
  tracks_init();
  ump3.changesetting('V', (uint8_t)10);
  Serial.print(track_table_sz);
  Serial.println(" tracks on the card");
  Serial.println(ump3.getsetting('V'));
}

int year_to_counts(int year) {
  return map(year, 1850, 2050, counts_1850, counts_2050);
}

int counts_to_year(int counts) {
  return map(counts, counts_1850, counts_2050, 1850, 2050);
}

void stepMotor() {
  digitalWrite(MOT_PIN,LOW);
  digitalWrite(MOT_PIN,HIGH);  
  delayMicroseconds(500);
}

char c;
int last_year = 0;
bool playing = false;
int encPos = 0;
char fname[16];

void loop() {
  encPos = readEncoder();

  int year = counts_to_year(encPos);
  if (year != last_year) {
    Serial.print("Year = ");
    Serial.println(year);
    last_year = year;

    displayYear(year*10);
    
    if (playing) {
      stopPlay();
      playing = false;
    }
    
    DateCode dc(year);
    int idx = find_track_idx(dc);
    if (idx >= 0) {
      Serial.print("Playing track: ");
      track_table[idx].get_filename(fname);
      Serial.println(fname);
      playTrack(idx);
      playing = true;
    }
    
  }
  
  if (Serial.available()) {
    c = Serial.read();
    switch(c) {
      case '\r': {
        int steps = calculateStepsFromLimit();
        Serial.println(steps);
      }
      break;
    }
  }  
}

// Sends Dial to Limit Switch, Counting Steps
// Returns counts
int calculateStepsFromLimit() {
  int counts = 0;
  
  int stop_value = digitalRead(MOT_LIM_PIN);

  digitalWrite(MOT_DIR_PIN, LOW); // Set motor direction down
  
  while(stop_value == HIGH) {
    stepMotor();
    counts++;
    stop_value = digitalRead(MOT_LIM_PIN);
  }  
  return counts;
}

void SendDialToLimitSwitch() {
  int stop_value = digitalRead(MOT_LIM_PIN);

  digitalWrite(MOT_DIR_PIN, LOW); // Set motor direction down
  
  while(stop_value == HIGH) {
    stepMotor();
    stop_value = digitalRead(MOT_LIM_PIN);
  }  
}

void SendDialToCountsFromLimitSwitch(int val) {
  digitalWrite(MOT_DIR_PIN, HIGH);
  while(val > 0) {
    stepMotor();
    val--;
  }
}

int b_pin = LOW; // To prevent re-allocation in the ISR 
void encoderISR() {
  // we assume a is low because this is a "falling" routine
  // and we should have timely interrupt processing
  b_pin = digitalRead(ENCODER_B_PIN);
  if (b_pin == HIGH) { // clockwise tick
    if (encoderPos < COUNTS_MAX) {
      encoderPos++;
      digitalWrite(MOT_DIR_PIN, HIGH);
      digitalWrite(MOT_PIN,LOW);
      digitalWrite(MOT_PIN,HIGH);
    } 
  } else { // counter-clockwise tick
    if (encoderPos > COUNTS_MIN) {
      encoderPos--;
      digitalWrite(MOT_DIR_PIN, LOW);
      digitalWrite(MOT_PIN,LOW);
      digitalWrite(MOT_PIN,HIGH);
    }
  }
}

int e;
int readEncoder() {
  noInterrupts();
  e = encoderPos;
  interrupts();
  return e;
}

void playTrack(int idx) {
  play_track_idx(idx);
  setVolume(65, 0xaa);
  setVolume(0, 0xa9);
}

void stopPlay() {
  setVolume(0, 0xaa);
  setVolume(65, 0xa9);
  ump3.stop();
}

//===================================================
//Audio pot stuff
//===================================================
void setVolume(byte vol, byte pot) {
  Wire.beginTransmission(POT_1_ADDY);
  Wire.write(pot);
  Wire.write(vol);
  
  Wire.endTransmission();
}

//===================================================
//FM Radio Stuff
//===================================================

//Copied from SparkFun Example.
//Define the register names
#define POWERCFG  0x02
#define CHANNEL  0x03
#define SYSCONFIG1  0x04
#define SYSCONFIG2  0x05
#define SYSCONFIG3  0x06
#define STATUSRSSI  0x0A
#define READCHAN  0x0B

//Register 0x02 - POWERCFG
#define SMUTE  15
#define DMUTE  14
#define SKMODE  10
#define SEEKUP  9
#define SEEK  8

//Register 0x03 - CHANNEL
#define TUNE  15

//Register 0x04 - SYSCONFIG1
#define RDS  12
#define DE  11

//Register 0x05 - SYSCONFIG2
#define SPACE1  5
#define SPACE0  4

//Register 0x0A - STATUSRSSI
#define STC  14
#define SFBL  13

#define DOWN  0 //Direction used for seeking. Default is down
#define UP  1

uint16_t fm_registers[16];

void fm_init() {
  Serial.println("Initializing FM");
  
  pinMode(FM_RST_PIN, OUTPUT);
  pinMode(SDIO_PIN, OUTPUT);
  digitalWrite(SDIO_PIN, LOW); //Low to tell the radio 2-wire mode
  digitalWrite(FM_RST_PIN, LOW); //Reset FM Module
  delay(1); //Some delays while we allow pins to settle
  digitalWrite(FM_RST_PIN, HIGH); //Bring DM out of reset with SDIO set to low and SEN pulled high with on-board resistor
  delay(1);
  
  Wire.begin();
  Wire.setClock(400000);
  
  fm_readRegisters(); //Read the current register set
  fm_registers[0x07] = 0x8100; //Enable the oscillator, from AN230 page 9, rev 0.61
  fm_updateRegisters(); //Update
 
  
  delay(500); //Wait for clock to settle - from AN230 page 9

  fm_readRegisters(); //Read the current register set
  fm_registers[POWERCFG] = 0x4001; //Enable the IC
  fm_registers[POWERCFG] |= 0x2000; //Set mono
  //  fm_registers[POWERCFG] |= (1<<SMUTE) | (1<<DMUTE); //Disable Mute, disable softmute
  fm_registers[SYSCONFIG1] |= (1<<RDS); //Enable RDS

  fm_registers[SYSCONFIG2] &= ~(1<<SPACE1 | 1<<SPACE0) ; //Force 200kHz channel spacing for USA

  fm_registers[SYSCONFIG2] &= 0xFFF0; //Clear volume bits
  fm_registers[SYSCONFIG2] |= 0x000F; //Set volume 
  
  fm_registers[SYSCONFIG3] |= 0x0034; // Set seek to be more strict
  fm_updateRegisters(); //Update

  delay(110);
}


bool fm_updateRegisters(void) {

  Wire.beginTransmission(FM_ADDY);
  //A write command automatically begins with register 0x02 so no need to send a write-to address
  //First we send the 0x02 to 0x07 control registers
  //In general, we should not write to registers 0x08 and 0x09
  for(int regSpot = 0x02 ; regSpot < 0x08 ; regSpot++) {
    byte high_byte = fm_registers[regSpot] >> 8;
    byte low_byte = fm_registers[regSpot] & 0x00FF;

    Wire.write(high_byte); //Upper 8 bits
    Wire.write(low_byte); //Lower 8 bits
  }

  //End this transmission
  byte ack = Wire.endTransmission();
 if(ack != 0) { //We have a problem! 
    Serial.print("Write Fail:"); //No ACK!
    Serial.println(ack, DEC); //I2C error: 0 = success, 1 = data too long, 2 = rx NACK on address, 3 = rx NACK on data, 4 = other error
    return false;
  }

  return true;
}

//Read the entire register control set from 0x00 to 0x0F
void fm_readRegisters(void){

  //FM begins reading from register upper register of 0x0A and reads to 0x0F, then loops to 0x00.
  Wire.requestFrom(FM_ADDY, 32); //We want to read the entire register set from 0x0A to 0x09 = 32 bytes.

  while(Wire.available() < 32) ; //Wait for 16 words/32 bytes to come back from slave I2C device
  //We may want some time-out error here

  //Remember, register 0x0A comes in first so we have to shuffle the array around a bit
  for(int x = 0x0A ; ; x++) { //Read in these 32 bytes
    if(x == 0x10) x = 0; //Loop back to zero
    fm_registers[x] = Wire.read() << 8;
    fm_registers[x] |= Wire.read();
    if(x == 0x09) break; //We're done!
  }
}



bool fm_seek(byte seekDirection){
  fm_readRegisters();

  //Set seek mode wrap bit
  fm_registers[POWERCFG] |= (1<<SKMODE); //Allow wrap
  //fm_registers[POWERCFG] &= ~(1<<SKMODE); //Disallow wrap - if you disallow wrap, you may want to tune to 87.5 first

  if(seekDirection == DOWN) fm_registers[POWERCFG] &= ~(1<<SEEKUP); //Seek down is the default upon reset
  else fm_registers[POWERCFG] |= 1<<SEEKUP; //Set the bit to seek up

  fm_registers[POWERCFG] |= (1<<SEEK); //Start seek

  fm_updateRegisters(); //Seeking will now start

  //Poll to see if STC is set
  while(1) {
    fm_readRegisters();
    if((fm_registers[STATUSRSSI] & (1<<STC)) != 0) break; //Tuning complete!

    Serial.print("Trying station:");
    Serial.println(fm_readChannel());
  }

  fm_readRegisters();
  int valueSFBL = fm_registers[STATUSRSSI] & (1<<SFBL); //Store the value of SFBL
  fm_registers[POWERCFG] &= ~(1<<SEEK); //Clear the seek bit after seek has completed
  fm_updateRegisters();

  //Wait for the si4703 to clear the STC as well
  while(1) {
    fm_readRegisters();
    if( (fm_registers[STATUSRSSI] & (1<<STC)) == 0) break; //Tuning complete!
    Serial.println("Waiting...");
  }

  if(valueSFBL) { //The bit was set indicating we hit a band limit or failed to find a station
    Serial.println("Seek limit hit"); //Hit limit of band during seek
    return false;
  }

  Serial.println("Seek complete"); //Tuning complete!
  return true;
}

int fm_readChannel(void) {
  fm_readRegisters();
  int channel = fm_registers[READCHAN] & 0x03FF; //Mask out everything but the lower 10 bits


  //Freq(MHz) = 0.200(in USA) * Channel + 87.5MHz
  //X = 0.2 * Chan + 87.5
  channel *= 2; //49 * 2 = 98


  channel += 875; //98 + 875 = 973
  return(channel);
}

void gotoChannel(int newChannel){
  //Freq(MHz) = 0.200(in USA) * Channel + 87.5MHz
  //97.3 = 0.2 * Chan + 87.5
  //9.8 / 0.2 = 49
  newChannel *= 10; //973 * 10 = 9730
  newChannel -= 8750; //9730 - 8750 = 980


  newChannel /= 20; //980 / 20 = 49


  //These steps come from AN230 page 20 rev 0.5
  fm_readRegisters();
  fm_registers[CHANNEL] &= 0xFE00; //Clear out the channel bits
  fm_registers[CHANNEL] |= newChannel; //Mask in the new channel
  fm_registers[CHANNEL] |= (1<<TUNE); //Set the TUNE bit to start
  fm_updateRegisters();

  //delay(60); //Wait 60ms - you can use or skip this delay

  //Poll to see if STC is set
  Serial.println("Tuning");
  while(1) {
    fm_readRegisters();
    if( (fm_registers[STATUSRSSI] & (1<<STC)) != 0) break; //Tuning complete!
  }
  Serial.println("Tuning Complete");
  
  fm_readRegisters();
  fm_registers[CHANNEL] &= ~(1<<TUNE); //Clear the tune after a tune has completed
  fm_updateRegisters();

  //Wait for the si4703 to clear the STC as well
  while(1) {
    fm_readRegisters();
    if( (fm_registers[STATUSRSSI] & (1<<STC)) == 0) break; //Tuning complete!
    Serial.println("Waiting...");
  }
}

//===================================================
//Display Stuff
//===================================================
void displayYear(long yr) {
  int digit1 = 0;
  int digit2 = 0;
  byte out = 0;
  
  digit1 = (yr/1000) % 10;
  digit2 = (yr/100) % 10;
  
  out = digit1 << 4;
  out = out | digit2;
  Wire.beginTransmission(IO_1_ADDY);
  Wire.write(0x13);
  Wire.write(out);
  Wire.endTransmission();

  digit1 = (yr/10) % 10;
  digit2 = yr % 10;
  
  out = digit1 << 4;
  out = out | digit2;
  Wire.beginTransmission(IO_2_ADDY);
  Wire.write(0x13);
  Wire.write(out);
  Wire.endTransmission();
  
  digit1 = yr/10000;
  
  out = digit1 << 4;
  Wire.beginTransmission(IO_2_ADDY);
  Wire.write(0x12);
  Wire.write(out);
  Wire.endTransmission();
  
}

void display_init() {
  Wire.beginTransmission(IO_1_ADDY);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
  Wire.beginTransmission(IO_2_ADDY);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
}

