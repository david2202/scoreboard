/*
 * This sketch drives the large scoreboard using an Arduino Uno.
 * It receives the score from the remote via. the NRF24L01.  It then 
 * writes the score out to the LEDs via. TPIC6B595 shift registers 
 * that have been daisy chained together.  Only the NRF24L01 is on 
 * the SPI bus, "bit banging" is used for the TPIC6B595.
 *
 * Current reading for 12 LED (10cm) is 54mA. Sink capacity of TPIC6B595
 * is 150mA per output.
 *
 * LED segment pattern is:
 *
 *       1
 *   +++++++++
 *   +       +
 * 2 +       + 3
 *   +   4   +
 *   +++++++++
 *   +       +
 * 5 +       + 6
 *   +   7   +
 *   +++++++++  + 8
 *
 * Only seven outputs from each TPIC6B595 are needed to drive the numbers.
 * The eighth is spare for other purposes.
 *
 * See http://forum.arduino.cc/index.php?topic=167887.0
 * See http://arduino-info.wikispaces.com/Nrf24L01-2.4GHz-HowTo
 * See https://github.com/maniacbug/RF24
 *
 */
#include <SPI.h>
#include "nRF24L01.h"
#include "RF24.h"
#include <ScoreboardCommon.h>

const int DATAPIN = 2;  // MOSI
const int CLOCKPIN = 3; // SPI CSLK
const int LATCHPIN = 4; // SPI SS

const int SCOREBOARD_DIGITS = 13;
const int TARGET_RUN_RATE_SWITCH = 10000; // Millis between switching between target score and run rate

// Output patterns for the digits from 0 to 9
const byte NUMBER[10] = {B01110111, B00100100, B01011101, B01101101, B00101110, B01101011, B01111011, B00100101, B01111111, B01101111};
const byte NUMBER_ANIM[3][10] = {
  {B01000000, B00000000, B01000000, B01000000, B00000000, B01000000, B01000000, B01000000, B01000000, B01000000},
  {B01110000, B00100000, B01101000, B01101000, B01110000, B01011000, B01010000, B00101000, B01111000, B01111000},
  {B01110111, B00100100, B01011101, B01101101, B00101110, B01101011, B01111011, B00100101, B01111111, B01101111}};
const int ANIM_DELAY = 100;

// Set up nRF24L01 radio on SPI bus plus pins 9 & 10 (as we are using a Uno)
RF24 radio(9,10);

score_t score;

int oldDigits[SCOREBOARD_DIGITS];
int newDigits[SCOREBOARD_DIGITS];
byte animationStep = 0;
unsigned long animTime = 0;

unsigned long targetRunRateSwitchTime = 0;  // When was the last time we switched between target score and run rate
boolean targetDisplayed = false;   // Are we currently displaying the target score?

void setup() {
  Serial.begin(115200);
  // Set the three SPI pins to be outputs:
  pinMode(DATAPIN, OUTPUT);
  pinMode(CLOCKPIN, OUTPUT);  
  pinMode(LATCHPIN, OUTPUT);

  // Setup the nRF24L01
  SPI.begin();
  radio.begin();
  
  radio.setRetries(15,15);

  radio.setPayloadSize(sizeof(score));

  // Open pipes to other nodes for communication
  radio.openWritingPipe(PIPES[1]);    // Open 'our' pipe for writing
  radio.openReadingPipe(1,PIPES[0]);  // Open the 'other' pipe for reading, in position #1

  radio.startListening();
  updateNewDigits();
}

void loop() {
  // Check if we have received a score
  if (radio.available()) {
    boolean ok = radio.read(&score, sizeof(score));
    //displayScoreSerial();
    if (score.target == 0) {
      targetDisplayed = false;
    }
    animationStep = 0;
    updateNewDigits();
  } else if (score.target != 0 && targetRunRateSwitchTime + TARGET_RUN_RATE_SWITCH < millis()) {
    // Time to switch target score and run rate
    targetDisplayed = !targetDisplayed;
    updateNewDigits();
    targetRunRateSwitchTime = millis();
  }
  displayScore();
}

void updateNewDigits() {
  // Update the new digits with the current score
  if (score.wickets < 10) {
    writeDigits(newDigits, score.wickets, 1, 0);
  } else {
    // If ten wickets, then don't display wickets
    newDigits[0] = -1;
  }
  writeDigits(newDigits, score.runs, 3, 1);
  writeDigits(newDigits, score.overs, 2, 4);
  writeDigits(newDigits, score.balls, 1, 6);
  writeDigits(newDigits, score.wideNBThisOver, 1, 7);
  writeDigits(newDigits, score.extras, 2, 8);
  if (targetDisplayed) {
    writeDigits(newDigits, score.target, 3, 10);
  } else {
    // float i = ((score.runs / ((score.overs * 6.0) + score.balls)) * 6.0 * 10.0) + 0.5;
    float i;
    if (score.target > 0) {
      i = score.targetRunRate() * 10 + 0.5;
    } else {
      i = score.currentRunRate() * 10 + 0.5;
    }
    
    int j = i;
    byte whole = j / 10;
    byte fraction = j % 10;
    writeDigits(newDigits, whole, 2, 10);
    writeDigits(newDigits, fraction, 1, 12);
  }
}

/*
 * Displays the score on the LED scoreboard.
 */
void displayScore() {
  if (equals(newDigits, oldDigits)) {
    // Display is up to date
    return;
  }

  if (animTime + ANIM_DELAY > millis()) {
    // Not ready for the next animation step
    return;
  }
  // Keep track of time we last animated
  animTime = millis();

  // Setup the stream of bytes to write to the shift registers
  byte stream[SCOREBOARD_DIGITS];

  // Write out the digits to the stream, using the current animation position
  for (int i = 0; i < SCOREBOARD_DIGITS; i++) {
    if (newDigits[i] < 0) {
      // Blank digit
      stream[i] = 0;
    } else {
      if (newDigits[i] == oldDigits[i]) {
        // If numbers haven't changed then use the last animation step (ie. the final number)
        stream[i] = NUMBER_ANIM[2][newDigits[i]];
      } else {
        // Use the current animation step
        stream[i] = NUMBER_ANIM[animationStep][newDigits[i]];
      }
    }
  }

  // Setup the last bits in the stream which are used for slashes and dots
  if (score.wickets < 10) {
    // Turn on the slash between the wickets and runs
    bitWrite(stream[0], 7, 1);
    bitWrite(stream[1], 7, 1);
  } else {
    // Turn the display of the number of wickets off
    stream[0] = 0;
  }
  // Turn on the decimal point
  bitWrite(stream[5], 7, 1);

  // Turn off wides/NB if they are zero
  if (score.wideNBThisOver == 0) {
    stream[7] = 0;
  }

  if (!targetDisplayed) {
    bitWrite(stream[11], 7, 1);
  }
  
  // Write the byte stream to the shift registers  
  digitalWrite(LATCHPIN, LOW);
  for (int i = SCOREBOARD_DIGITS; i >= 0; i--) {
    shiftOut(DATAPIN, CLOCKPIN, MSBFIRST, stream[i]);
  }
  digitalWrite(LATCHPIN, HIGH);
  if (animationStep == 2) {
    // Finished the animation so copy the new digits to the old digits
    for (int i = 0; i < SCOREBOARD_DIGITS; i++) {
      oldDigits[i] = newDigits[i];
    }
    // Reset the animation step
    animationStep = 0;
  } else {
    // Move to the next animation step
    animationStep++;
  }
}

/*
 * Writes the binary representation of a number to the specified stream at
 * the specified offset.  The length specifies how many bytes will be
 * written.
 */
void writeNumber(byte stream[], byte value, byte len, byte offset) {
  for (int i = 0; i < len; i++) {
    const byte pos = offset + len - i - 1;
    // If there are still digits to write or this is the first digit
    if (value >= pow(10, i) || i ==0) {
      stream[pos] = NUMBER[digit(value, i + 1, len)];
    } else {
      stream[pos] = B00000000;
    }
  }
}

/*
 * Writes the digits of a number to the specified array at
 * the specified offset.  The length specifies how many bytes will be
 * written.
 */
void writeDigits(int digits[], byte value, byte len, byte offset) {
  for (int i = 0; i < len; i++) {
    const byte pos = offset + len - i - 1;
    // If there are still digits to write or this is the first digit
    if (value >= pow(10, i) || i ==0) {
      digits[pos] = digit(value, i + 1, len);
    } else {
      digits[pos] = -1;
    }
  }
}

/*
 * Returns the specified digit from the specified value
 */
byte digit(int value, int digit, int len) {
  int whole = 0;
  int carry = value;
  boolean displayZero = false;
  
  for (int i = len - 1; i >=0; i--) {
    whole = carry / (pow(10, i));
    if (i + 1 == digit) {
        return whole;
    }
    carry = carry - (whole * pow(10, i));
  }
}

boolean equals(int array1[], int array2[]) {
  for (int i = 0; i < SCOREBOARD_DIGITS; i++) {
    if (array1[i] != array2[i]) {
      return false;
    }
  }
  return true;
}

/*
 * Debugging function to display the score to the serial port
 */
void displayScoreSerial() {
  Serial.print(score.wickets);
  Serial.print("/");
  Serial.print(score.runs);
  Serial.print(" ");
  Serial.print("Over: ");
  Serial.print(score.overs);
  Serial.print(".");
  Serial.print(score.balls);
  Serial.print(" ");
  if (score.wideNBThisOver > 0) {
    Serial.print("(");
    Serial.print(score.wideNBThisOver);
    Serial.print(") ");
  }
  Serial.print("Extras: " );
  Serial.print(score.extras);
  Serial.print(" Target: ");
  Serial.print(score.target);
  Serial.println();
  Serial.print(F("Target Overs: "));
  Serial.print(score.targetOvers);
  Serial.print(F("."));
  Serial.print(score.targetBalls);
  Serial.println();
}

