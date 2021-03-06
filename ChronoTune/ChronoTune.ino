// Chronotune V2.0
// Frank Palazzolo
// Based on original TimeRadio code from Eric Merrill et al.

#include <Wire.h>
#include <EEPROM.h>
#include <RogueMP3.h>

#define ump3_serial Serial1 
RogueMP3 ump3(ump3_serial);

#include "TrackManager.h"

/////// Values which need to change if you re-mount/change the limit switch or dial ///////

// Where the dial stops normally
#define YEARPOS_MIN   (1840*65536)
#define YEARPOS_MAX   (2060*65536)

// Used for calibration - use debug serial input to convert from counts to dial position
#define COUNTS_1850  416
#define COUNTS_2050 2786

// Hard Limit during calibration
#define COUNTS_MAX  2900

// Change for the new year
#define THIS_YEAR 2016

// Radio station used for "static" sound (FM Mhz * 10)
#define RADIO_STATION_STATIC  911
// Radio station used for current year (FM Mhz * 10)
#define RADIO_STATION_LIVE    947

// Time (seconds) to linger after a track is done playing
#define LINGER_MP3_DONE_TIME 5
// Time (seconds) to linger on a dead station
#define LINGER_RADIO_STATIC_TIME 20

/////// ATmega2560 pin connections ///////

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

/////// I2C addresses ///////

#define FM_ADDY 0x10
#define POT_1_ADDY 0x28
#define POT_2_ADDY 0x29

#define IO_1_ADDY 0x26
#define IO_2_ADDY 0x27

// yearPos - written by ISR, read by main loop
volatile long yearPos = 0;
volatile long increment = 0;

volatile bool using_i2c = false;

// Enum for Chronotune State
typedef enum {
  STATE_INIT = 0,
  STATE_RADIO_STATIC,
  STATE_MP3_PLAYING,
  STATE_MP3_DONE,
  STATE_RADIO_PLAYING,
} PlayStateT;

// TrackManager
TrackManager tm;

void setup() {
  
  Serial.begin(57600);  // Debug  
  ump3_serial.begin(9600);  // ump3 board
  ump3.sync();
  ump3.stop();
  
  // Rotary Encoder
  pinMode(ENCODER_A_PIN, INPUT);
  pinMode(ENCODER_B_PIN, INPUT);

  // Display
  display_init();
  displayYear(8888);
  
  // Limit switch
  pinMode(MOT_LIM_PIN, INPUT);
  digitalWrite(MOT_LIM_PIN, HIGH);  // Pull-up, active low

  // Stepper Motor Init
  pinMode(MOT_PIN, OUTPUT);
  digitalWrite(MOT_PIN, LOW);
  pinMode(MOT_DIR_PIN, OUTPUT);
  digitalWrite(MOT_DIR_PIN, LOW);

  // encoderPos is not "live" yet, reset it to the current dial position
  int encoderPos = calculateStepsFromLimit();
  if (encoderPos > COUNTS_MAX) 
    encoderPos = COUNTS_MAX;
  SendDialToCountsFromLimitSwitch(encoderPos);

  // Initialize encoder values
  increment = (2050-1850)*65536/(COUNTS_2050-COUNTS_1850); 
  yearPos = 1850*65536+increment*encoderPos-increment*COUNTS_1850;

  displayYear(readYear());
  
  //FM
  fm_init();
  gotoChannel(RADIO_STATION_STATIC);
  setVolume(0, 0xaa); //FM

  // MP3
  setVolume(65, 0xa9);
  tracks_init();
  ump3.setVolume(10);

  // Encoder Interrupt Service Routine
  attachInterrupt(digitalPinToInterrupt(ENCODER_A_PIN), encoderISR, FALLING); 
}

// State Machine

int current_index = 0;
PlayStateT gPlayState = STATE_INIT; 
char current_track_name[FILE_NAME_MAX_SZ];

void GoToState(PlayStateT ps) {
  if ((gPlayState == ps) && (ps != STATE_MP3_PLAYING))
    return;
  switch(ps) {
    case STATE_RADIO_STATIC:
      gotoChannel(RADIO_STATION_STATIC);
      if ((gPlayState == STATE_MP3_PLAYING) || (gPlayState == STATE_MP3_DONE) || (gPlayState == STATE_INIT)) {
        // Switch to FM
        setVolume(0, 0xaa); //FM
        setVolume(65, 0xa9); // MP3
        ump3.stop();
      }
    break;
    case STATE_RADIO_PLAYING:
      gotoChannel(RADIO_STATION_LIVE);
      if ((gPlayState == STATE_MP3_PLAYING) || (gPlayState == STATE_MP3_DONE) || (gPlayState == STATE_INIT)) {
        // Switch to FM
        setVolume(0, 0xaa); //FM
        setVolume(65, 0xa9); // MP3
        ump3.stop();   
      }        
    break;
    case STATE_MP3_PLAYING:
      ump3.playFile(current_track_name);
      if ((gPlayState == STATE_RADIO_STATIC) || (gPlayState == STATE_RADIO_PLAYING) || (gPlayState == STATE_MP3_DONE) || (gPlayState == STATE_INIT)) {
        // Switch to MP3
        setVolume(65, 0xaa);
        setVolume(0, 0xa9);
      }
      Serial.print("Playing track: ");
      Serial.println(current_track_name);
    break;
    case STATE_MP3_DONE:
      // Switch to FM
      setVolume(0, 0xaa); //FM
      setVolume(65, 0xa9); // MP3
      ump3.stop();
    break;
  }
  gPlayState = ps;
}

int last_year = 0;
volatile int seek_year = 0;

long seconds = 0;
long millisecs = 0;

void UpdateIdleTimer()
{
  static long last_millis = millis();
  
  long current = millis();
  long diff = current - last_millis;
  last_millis = current;
  
  millisecs += diff;
  if (millisecs > 1000) {
    seconds += millisecs/1000;
    millisecs = millisecs % 1000;
  }
}

int idle_seconds = 0;

void IdlePoll()
{
  static long last_seconds = 0;
  if (seconds != last_seconds)
  {
    idle_seconds++;
    last_seconds = seconds;
  }
  if ((gPlayState != STATE_MP3_PLAYING) && (gPlayState != STATE_MP3_DONE) && (gPlayState != STATE_RADIO_STATIC)) {
    idle_seconds = 0;
  } else if (gPlayState == STATE_MP3_PLAYING) {
    if (idle_seconds > 4) {
      idle_seconds = 0;
      if (!ump3.isPlaying())
        GoToState(STATE_MP3_DONE);
    }
  } else if (gPlayState == STATE_MP3_DONE) {
    if (idle_seconds > LINGER_MP3_DONE_TIME) {
      idle_seconds = 0;
      Serial.print("Bored! Trying ");
      int temp_seek_year = tm.GetRandomYear();
      while (temp_seek_year == last_year)
        temp_seek_year = tm.GetRandomYear();
      Serial.println(temp_seek_year);
      writeSeekYear(temp_seek_year);
    }
  } else if (gPlayState == STATE_RADIO_STATIC) {
    if (idle_seconds > LINGER_RADIO_STATIC_TIME) {
      idle_seconds = 0;
      Serial.print("Bored! Trying ");
      int temp_seek_year = tm.GetRandomYear();
      while (temp_seek_year == last_year)
        temp_seek_year = tm.GetRandomYear();
      Serial.println(temp_seek_year);
      writeSeekYear(temp_seek_year);
    }
  }
}

void DoSeeking()
{
  int syear = 0;
  int year = last_year;

  syear = readSeekYear();
  
  while((syear != 0) && (syear != year))
  {
    noInterrupts();
    if (syear > year)
      moveDial(HIGH);
    else if (syear < year)
      moveDial(LOW);
    interrupts();
    delayMicroseconds(1500);
    syear = readSeekYear();
    year = readYear();
  }
}

char c;

void loop() {
  
  int year = readYear();
  if (year != last_year) {
    Serial.print("Year = ");
    Serial.println(year);
    
    last_year = year;

    if (year != readDisplayYear()) {
      using_i2c = true;
      displayYear(year);
      using_i2c = false;
    }
    
    if (year == THIS_YEAR) {
      GoToState(STATE_RADIO_PLAYING);
    } else {
      bool found = tm.GetRandomTrack(current_track_name, year);
      if (found) {
        GoToState(STATE_MP3_PLAYING);
      } else {
        GoToState(STATE_RADIO_STATIC);
      }
    }
  }

  UpdateIdleTimer();

  IdlePoll();

  DoSeeking();

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

// Stepper Motor Control Routines

void stepMotor() {
  digitalWrite(MOT_PIN,LOW);
  digitalWrite(MOT_PIN,HIGH);  
  delayMicroseconds(500);
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

// Encoder routines

volatile int display_year = 0;

int b_pin = LOW; // To prevent re-allocation in the ISR 
void encoderISR() {
  // we assume a is low because this is a "falling" routine
  // and we should have timely interrupt processing
  b_pin = digitalRead(ENCODER_B_PIN);
  seek_year = 0;
  moveDial(b_pin);
}

void moveDial(int dir)
{
  if (dir == HIGH) { // clockwise tick
    if (yearPos < YEARPOS_MAX) {
      yearPos+=increment;
      digitalWrite(MOT_DIR_PIN, HIGH);
      digitalWrite(MOT_PIN,LOW);
      digitalWrite(MOT_PIN,HIGH);
    } 
  } else { // counter-clockwise tick
    if (yearPos > YEARPOS_MIN) {
      yearPos-=increment;
      digitalWrite(MOT_DIR_PIN, LOW);
      digitalWrite(MOT_PIN,LOW);
      digitalWrite(MOT_PIN,HIGH);
    }
  }
  if (using_i2c) return;
  if (display_year != yearPos>>16) {
    display_year = yearPos>>16;
    detachInterrupt(digitalPinToInterrupt(ENCODER_A_PIN));
    interrupts();
    displayYear(display_year);
    noInterrupts();
    attachInterrupt(digitalPinToInterrupt(ENCODER_A_PIN), encoderISR, FALLING);
    interrupts(); 
  }
}

int y;
int readYear() {
  noInterrupts();
  y = yearPos>>16;
  interrupts();
  return y;
}

int s;
int readSeekYear() {
  noInterrupts();
  s = seek_year;
  interrupts();
  return s;
}

int writeSeekYear(int s) {
  noInterrupts();
  seek_year = s;
  interrupts();
}

int d;
int readDisplayYear() {
  noInterrupts();
  d = display_year;
  interrupts();
  return d;
}
//===================================================
//Audio pot stuff
//===================================================
void setVolume(byte vol, byte pot) {
  using_i2c = true;
  Wire.beginTransmission(POT_1_ADDY);
  Wire.write(pot);
  Wire.write(vol);
  Wire.endTransmission();
  using_i2c = false;
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
  using_i2c = true;
  int savedChannel = newChannel;
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
  Serial.print("Tuning to ");
  Serial.println(savedChannel);
  while(1) {
    fm_readRegisters();
    if( (fm_registers[STATUSRSSI] & (1<<STC)) != 0) break; //Tuning complete!
  }
  
  fm_readRegisters();
  fm_registers[CHANNEL] &= ~(1<<TUNE); //Clear the tune after a tune has completed
  fm_updateRegisters();

  //Wait for the si4703 to clear the STC as well
  while(1) {
    fm_readRegisters();
    if( (fm_registers[STATUSRSSI] & (1<<STC)) == 0) break; //Tuning complete!
    //Serial.println("Waiting...");
  }
  using_i2c = false;
}

//===================================================
//Display Stuff
//===================================================
void displayYear(long yr) {
  int digit1 = 0;
  int digit2 = 0;
  int digit3 = 0;
  int digit4 = 0;
  byte out1 = 0;
  byte out2 = 0;

  digit4 = yr % 10;
  yr /= 10;
  digit3 = yr % 10;
  yr /= 10;
  digit2 = yr % 10;
  yr /= 10;
  digit1 = yr % 10;
  
  out1 = (digit2 << 4) | digit1;
  out2 = (digit3 << 4) | digit4;
  
  Wire.beginTransmission(IO_1_ADDY);
  Wire.write(0x13);
  Wire.write(out1);
  Wire.endTransmission();
  
  Wire.beginTransmission(IO_2_ADDY);
  Wire.write(0x13);
  Wire.write(out2);
  Wire.endTransmission();

#if 0
  digit1 = yr/10000;
  
  out = digit1 << 4;
  Wire.beginTransmission(IO_2_ADDY);
  Wire.write(0x12);
  Wire.write(out);
  Wire.endTransmission();
#endif
  
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

#define LINE_BUF_SZ 64

//
//  tracks_init
//
//  Initialize the interface to the mp3 board.
//  Also populates the track table.
//
int tracks_init(void)
{
  int idx;
  long baud;
  char buf[LINE_BUF_SZ];
  char file_name[FILE_NAME_MAX_SZ];

  int num_tracks = 0;

  idx = 0;
  ump3_serial.print("FC L /\r");
  
  while(ump3_serial.peek() != '>')
  {
    idx = 0;

    // read the whole line
    do
    {
      while(!ump3_serial.available());
      buf[idx++] = ump3_serial.read();
    } while(buf[idx-1] != 0x0D);
    
    // replace the trailing CR with a null
    buf[idx-1] = 0;
    
    //Serial.print("uMP3 rx: ");
    //Serial.println(buf);

    if( 1 == sscanf(buf, "%*d %s", &file_name))
    {
      tm.AddTrack(file_name);
      num_tracks++;
    }

    // wait here until next character is available
    while(!ump3_serial.available());
  }
  ump3_serial.read(); // read the '>'

  Serial.print(num_tracks);
  Serial.println(" tracks on the card");
  
  return 0;
}

