/*
 * Sketch for a cricket scoreboard.  Requires the following hardware:
 * - 1602 LCD display
 * - 7 pushbuttons
 * - NRF24L01 2.4 GHz transceiver
 * - WINC1500 Wifi breakout
 *
 * Scoreboard has concept of "plus" mode.  While in plus mode, any button presses won't
 * increment the number of balls.  This is used for recording multiple runs, multiple
 * leg byes or a no ball/wide plus runs.  Plus mode automatically turns off after three
 * seconds, but can be re-engaged by pressing the MODE button.
 *
 * TODO - Make the number of wides & no balls per over configurable
 * TODO - Add a temperature sensor - read temperature from the scoreboard arduino
 *        via the NRF24L01 and display on LCD
 */
#include <LiquidCrystal.h>

#include <EEPROMex.h>
#include <EEPROMVar.h>
#include <Buttons.h>
#include <ScoreboardCommon.h>
#include <SPI.h>
#include "nRF24L01.h"
#include "RF24.h"
#include <Adafruit_WINC1500.h>
#include <string.h>

// #define DEBUG

// Define the WINC1500 board connections below.
// If you're following the Adafruit WINC1500 board
// guide you don't need to modify these:
#define WINC_CS   48
#define WINC_IRQ  3
#define WINC_RST  47

// Setup the WINC1500 connection with the pins above and the default hardware SPI.
Adafruit_WINC1500 WiFi(WINC_CS, WINC_IRQ, WINC_RST);

//char ssid[] = "NETGEAR26";       //  your network SSID (name)
char ssid[] = "XT1068 4797";       //  your network SSID (name)
char pass[] = "rockycurtain281"; // your network password (use for WPA, or use as key for WEP)
int keyIndex = 0;                // your network key Index number (needed only for WEP)

int status = WL_IDLE_STATUS;
// if you don't want to use DNS (and reduce your sketch size)
// use the numeric IP instead of the name for the server:
//IPAddress server(141,101,112,175);  // numeric IP for test page (no DNS)
//char server[] = "192.168.0.96";    // domain name for test page (using DNS)
//int port = 8080;
char server[] = "scoreboard.zbtzhaj3sj.ap-southeast-2.elasticbeanstalk.com";
int port = 80;
boolean wifiConnected = false;

// Initialize the Ethernet client library
Adafruit_WINC1500Client client;

#define IDLE_TIMEOUT_MS  3000

// Constants
const byte NBR_OF_BUTTONS = 7;                   // Number of buttons pins
const int MODE_TIME = 3000;                      // Time (in millis) that plus mode lasts
const int DEBOUNCE_DELAY = 40;                   // Delay time for debouncing buttons
const int REPEAT_INITIAL_DELAY = 1000;           // How long button must be held down before it starts repeating
const int REPEAT_DELAY = 100;                    // How long between repeats

void inningsOrReset(Button* button);
void mode(Button* button);
void wicket(Button* button);
void run(Button* button);
void noScore(Button* button);
void legBye(Button* button);
void extra(Button* button);

// Button locations (relative to  buttonPin array)
Button* button[] = {new DigitalButton(22, INPUT_PULLUP, DEBOUNCE_DELAY, REPEAT_INITIAL_DELAY, REPEAT_DELAY, inningsOrReset),
                    new DigitalButton(23, INPUT_PULLUP, DEBOUNCE_DELAY, mode),
                    new DigitalButton(24, INPUT_PULLUP, DEBOUNCE_DELAY, REPEAT_INITIAL_DELAY, REPEAT_DELAY, wicket),
                    new DigitalButton(25, INPUT_PULLUP, DEBOUNCE_DELAY, REPEAT_INITIAL_DELAY, REPEAT_DELAY, run),
                    new DigitalButton(26, INPUT_PULLUP, DEBOUNCE_DELAY, REPEAT_INITIAL_DELAY, REPEAT_DELAY, noScore),
                    new DigitalButton(27, INPUT_PULLUP, DEBOUNCE_DELAY, REPEAT_INITIAL_DELAY, REPEAT_DELAY, legBye),
                    new DigitalButton(28, INPUT_PULLUP, DEBOUNCE_DELAY, REPEAT_INITIAL_DELAY, REPEAT_DELAY, extra) };

const byte INNINGS_RESET_BUTTON = 0;
const byte MODE_BUTTON = 1;
const byte WICKET_BUTTON = 2;
const byte RUN_BUTTON = 3;
const byte NO_SCORE_BUTTON =4;
const byte LEG_BYE_BUTTON = 5;
const byte EXTRA_BUTTON = 6;

// Variables
LiquidCrystal lcd(39, 38, 37, 36, 35, 34);      // create an lcd object and assign the pins

// Set up nRF24L01 radio on SPI bus plus pins 49 & 53 (as we are using a Mega)
RF24 radio(49,53);

long modeOnTime = 0;                            // The time plus mode was turned on
byte opMode = SCOREBOARD_OPMODE;
int currentMode = NO_MODE;                      // NO_MODE, PLUS_MODE or MINUS_MODE
boolean modeFirstPress = true;                  // Is it the first button press on entering the mode?
boolean scoreDirty = false;                     // Does the score need to be sent to the remote?
score_t score;

unsigned long sendTime = 0;

void setup() {
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }
  
  Serial.begin(115200);
  lcd.begin(16, 2);                             // Set the display to 16 columns and two rows
  lcd.clear();
  
  EEPROM.readBlock(0, score);                  // Read the saved score from EEPROM

  initialiseRadio();
  wifiConnected = initialiseWiFi();

  lcd.clear();
  displayScore();                               // Display the score for the first time
}

void loop(){
  readButtons();
  if (opMode == SCOREBOARD_OPMODE && (modeOnTime + MODE_TIME)                 // Check if mode has expired
    < millis()) {
    clearMode();
    if (scoreDirty) {
      lcd.setCursor(15, 1);
      lcd.print(char(B01011100));
      sendScoreRadio();
      if (wifiConnected) {
        sendScoreWiFi();
      }
      lcd.setCursor(15, 1);
      lcd.print(F(" "));
      sendTime = millis();
      scoreDirty = false;
    }
  }
  receiveResponseWiFi();
}

void readButtons() {
  for (int i = 0; i < NBR_OF_BUTTONS; i++) {   // Read the state of all buttons
    button[i]->read();
  }
}

void sendScoreRadio() {
  radio.stopListening();
  bool ok = radio.write(&score, sizeof(score));
  #ifdef DEBUG
    Serial.println(ok);
  #endif
  radio.startListening();
}

void sendScoreWiFi() {
  // if you get a connection, report back via serial:
  if (client.connect(server, port)) {
    #ifdef DEBUG
      Serial.println("send-begin");
    #endif
    String payload = F("{ \"wickets\": ");
    payload += score.wickets;
    payload += F(", \"runs\": ");
    payload += score.runs;
    payload += F(", \"overs\": ");
    payload += score.overs;
    payload += F(", \"balls\": ");
    payload += score.balls;
    payload += F(", \"wideNBThisOver\": ");
    payload += score.wideNBThisOver;
    payload += F(", \"extras\": ");
    payload += score.extras;
    payload += F(", \"target\": ");
    payload += score.target;
    payload += F("}");
    
    #ifdef DEBUG
      Serial.println(payload);
    #endif

    client.println(F("POST /score HTTP/1.1"));
    client.print(F("Host: ")); client.print(server); client.print(":"); client.println(String(port).c_str());
    client.println(F("Connection: keep-alive"));
    client.print(F("Content-Length: ")); client.println(String(payload.length()).c_str());
    client.println(F("Cache-Control: no-cache"));
    client.println(F("Content-Type: application/json"));
    client.println(F("Accept: */*"));
    client.println();
    client.print(payload.c_str());
    client.println();
    #ifdef DEBUG
      Serial.println("send-end");
    #endif
  } else {
    #ifdef DEBUG
      Serial.println(F("Connection failed"));    
    #endif
  }
  #ifdef DEBUG
    Serial.println(F("-------------------------------------"));  
  #endif
}

void receiveResponseWiFi() {
  // Read any data that is available
  while (client.available()) {
    char c = client.read();
    #ifdef DEBUG
      Serial.print(c);
    #endif
  }

  // Give up waiting and close connection
  if (client.connected() && millis() > sendTime + IDLE_TIMEOUT_MS) {
    #ifdef DEBUG
      Serial.println(F("-------------------------------------"));
    #endif
    client.stop();
  }
}

/*
 * Puts the scoreboard into "plus" mode
 */
void changeMode() {
  if (currentMode == NO_MODE) {                    // Not already in a mode (plus or minus)
    modeOnTime = millis();                         // Record the time we went into mode
  } else {
    if ((modeOnTime + MODE_TIME - millis())        // If within 2 seconds of end of mode
       < 2000) {
      modeOnTime += 2000;                          // Extend mode time by 2 seconds
    }
  }
}

/*
 * Clears the mode
 */
void clearMode() {
  currentMode = NO_MODE;
  modeFirstPress = true;
  if (opMode == SCOREBOARD_OPMODE) {
    lcd.setCursor(15, 1);
    lcd.print(F(" "));
  } else if (opMode == TARGET_OPMODE) {
    lcd.setCursor(15, 0);
    lcd.print(F(" "));
  }
  if (score.balls == 0                               // If it's the first ball of the over
      && !anyRepeating()) {                          // and we're not repeating (i.e. button held down)
    EEPROM.writeBlock(0, score);                  // Store the current score in EEPROM
  }
}

boolean anyRepeating() {
  for (int i = 0; i < NBR_OF_BUTTONS; i++) {
    if (button[i]->isRepeating()) {
      return true;
    }
  }
  return false;
}

/*
 * Sets the mode correctly after a button press.  If we are not in any mode,
 * then the new mode is plus mode, otherwise the mode doesn't change.
 */
void setModeAfterButtonPress() {
  changeMode();
  modeFirstPress = false;

  if (currentMode == NO_MODE) {
    currentMode = PLUS_MODE;
  }
}

/*
 * Handles the innings/reset button being pressed
 */
void inningsOrReset(Button* button) {
  opMode = (opMode + 1) % 4;
  lcd.clear();
  displayScore();                         // Update the scoreboard
}

/*
 * Changes the mode
 */
void mode(Button* button) {
  if (opMode == SCOREBOARD_OPMODE || opMode == TARGET_OPMODE) {
    changeMode();
    currentMode = (currentMode + 1) % 3;
    displayScore();                         // Update the scoreboard
  } else if (opMode == INNINGS_OPMODE) {
    innings();
    scoreDirty = true;
    opMode = TARGET_OPMODE;
    lcd.clear();
    displayScore();
  } else if (opMode == RESET_OPMODE) {
    reset();
    scoreDirty = true;
    lcd.clear();
    opMode = SCOREBOARD_OPMODE;
    displayScore();
  }
}

/*
 * Resets the score
 */
void reset() {
  score.runs = 0;
  score.extras = 0;
  score.wideNBThisOver = 0;
  score.wickets = 0;
  score.overs = 0;
  score.balls = 0;
  score.target = 0;
  score.targetOvers = 0;
  score.targetBalls = 0;
  clearMode();
  EEPROM.writeBlock(0, score);
}

/*
 * Change of innings. Makes the target score the current score and then resets
 * the current score.
 */
void innings() {
  int currentRuns = score.runs;
  int currentOvers = score.overs;
  int currentBalls = score.balls;
  reset();
  score.target = currentRuns;
  score.targetOvers = 40;
  score.targetBalls = 0;
  EEPROM.writeBlock(0, score);
}

/*
 * Records a run
 */
void run(Button* button) {
  if (opMode == SCOREBOARD_OPMODE) {
    score.run(currentMode);
    setModeAfterButtonPress();                // Go into plus mode
    displayScore();                           // Update the scoreboard
    scoreDirty = true;
  } else if (opMode == TARGET_OPMODE) {
    score.targetRunsChange(currentMode);
    setModeAfterButtonPress();
    displayScore();
    scoreDirty = true;
  }
}

/*
 * Records an extra (no ball/wide) which doesn't update the number of balls
 */
void extra(Button* button) {
  score.extra(currentMode, modeFirstPress);
  setModeAfterButtonPress();                       // Go into plus mode
  displayScore();                                  // Update the scoreboard
  scoreDirty = true;
}

/*
 * Records a leg bye
 */
void legBye(Button* button) {
  if (opMode == SCOREBOARD_OPMODE) {
    score.legBye(currentMode);
    setModeAfterButtonPress();                       // Go into plus mode
    displayScore();                                  // Update the scoreboard
    scoreDirty = true;
  }
}

/*
 * Records a wicket
 */
void wicket(Button* button) {
  if (opMode == SCOREBOARD_OPMODE) {
    score.wicket(currentMode);
    setModeAfterButtonPress();                      // Go into plus mode
    displayScore();                                 // Update the scoreboard
    scoreDirty = true;
  }
}

/*
 * Records a no score
 */
void noScore(Button* button) {
  if (opMode == SCOREBOARD_OPMODE) {
    score.noScore(currentMode);
    setModeAfterButtonPress();                     // Go into plus mode
    displayScore();                                // Update the scoreboard
    scoreDirty = true;
  } else if (opMode == TARGET_OPMODE) {
    score.targetBallsChange(currentMode);
    setModeAfterButtonPress();
    displayScore();
    scoreDirty = true;
  }
}

/*
 * Displays the score on the scoreboard
 */
void displayScore() {
  if (opMode == SCOREBOARD_OPMODE) {
    displayScoreboard();
  } else if (opMode == INNINGS_OPMODE) {
    displayInningsChange();
  } else if (opMode == TARGET_OPMODE) {
    displayTarget();
  } else if (opMode == RESET_OPMODE) {
    displayReset();
  }
}

void displayScoreboard() {
  lcd.setCursor(0, 0);
  lcd.print(F("S:"));
  if (score.wickets < 10) {
    lcd.print(score.wickets);
    lcd.print(F("/"));
  }
  lcd.print(score.runs);
  lcd.print(F("    "));
  lcd.setCursor(8, 0);
  lcd.print(F("O:"));
  lcd.print(score.overs);
  lcd.print(F("."));
  lcd.print(score.balls);
  lcd.print(F(" "));
  lcd.setCursor(15, 0);
  if (score.wideNBThisOver > 0) {
    lcd.print(score.wideNBThisOver);
  } else {
    lcd.print(F(" "));
  }
  lcd.setCursor(0, 1);
  lcd.print(F("X:"));
  lcd.print(score.extras);
  lcd.print(F("     "));
  lcd.setCursor(8, 1);
  lcd.print(F("T:"));
  lcd.print(score.target);
  lcd.print(F("        "));
  lcd.setCursor(15, 1);
  if (currentMode == PLUS_MODE) {
    lcd.print("+");
  } else if (currentMode == MINUS_MODE) {
    lcd.print("-");
  } else {
    lcd.print(" ");
  }
  //displayScoreSerial();  
}

void displayTarget() {
  lcd.setCursor(0, 0);
  lcd.print(F("T:"));
  lcd.print(score.target);
  lcd.print(F("   "));
  lcd.setCursor(8,0);
  lcd.print(F("O:"));
  lcd.print(score.targetOvers);
  lcd.print(F("."));
  lcd.print(score.targetBalls);
  lcd.print(F(" "));  
  lcd.setCursor(15, 0);
  if (currentMode == PLUS_MODE) {
    lcd.print("+");
  } else if (currentMode == MINUS_MODE) {
    lcd.print("-");
  } else {
    lcd.print(" ");
  }
  lcd.setCursor(0, 1);
  lcd.print(F("R=Runs NS=Overs"));
}

void displayInningsChange() {
  lcd.setCursor(0, 0);
  lcd.print(F("Confirm innings?"));
  lcd.setCursor(0, 1);
  lcd.print(F("+/-=Yes"));
}

void displayReset() {
  lcd.setCursor(0, 0);
  lcd.print(F("Confirm reset?"));
  lcd.setCursor(0, 1);
  lcd.print(F("+/-=Yes"));
}

/*
 * Debugging function to display the score to the serial port
 */
void displayScoreSerial() {
  Serial.print(score.wickets);
  Serial.print(F("/"));
  Serial.print(score.runs);
  Serial.print(" ");
  Serial.print(F("Over: "));
  Serial.print(score.overs);
  Serial.print(F("."));
  Serial.print(score.balls);
  Serial.print(F(" "));
  Serial.print(F("Extras: "));
  Serial.print(score.extras);
  Serial.print(F(" "));
  Serial.print(F("Target Overs: "));
  Serial.print(score.targetOvers);
  Serial.print(F("."));
  Serial.print(score.targetBalls);
  Serial.println();
}

void initialiseRadio() {
  lcd.setCursor(0, 0);
  lcd.print(F("Radio: Starting"));

  radio.begin();

  lcd.setCursor(7, 0);
  lcd.print(F("Started "));
  
  radio.setRetries(15,15);

  // optionally, reduce the payload size.  seems to
  // improve reliability
  radio.setPayloadSize(sizeof(score));

  //
  // Open pipes to other nodes for communication
  //

  // This simple sketch opens two pipes for these two nodes to communicate
  // back and forth.
  // Open 'our' pipe for writing
  // Open the 'other' pipe for reading, in position #1 (we can have up to 5 pipes open for reading)
  radio.openWritingPipe(PIPES[0]);
  lcd.setCursor(7, 0);
  lcd.print(F("Writing "));
  radio.openReadingPipe(1,PIPES[1]);
  lcd.setCursor(7, 0);
  lcd.print(F("Reading "));

  radio.startListening();
  lcd.setCursor(7, 0);
  lcd.print(F("Complete"));
}

boolean initialiseWiFi() {
  lcd.setCursor(0, 1);
  lcd.print(F("WiFi:  Starting"));
  #ifdef DEBUG
    Serial.println(F("Hello, WINC1500!"));
    Serial.println(F("Initializing..."));
  #endif

  // check for the presence of the shield:
  if (WiFi.status() == WL_NO_SHIELD) {
    #ifdef DEBUG
      Serial.println("WiFi shield not present");
    #endif
    lcd.setCursor(7, 1);
    lcd.print(F("Not present"));
    delay(5000);
    return false;
  }

  // attempt to connect to Wifi network:
  lcd.setCursor(7, 1);
  lcd.print(ssid);
  lcd.print(F("        "));
  #ifdef DEBUG
    Serial.print(F("Attempting to connect to SSID: "));
    Serial.println(ssid);
  #endif
  
  // Connect to WPA/WPA2 network
  status = WiFi.begin(ssid, pass);

  // wait 10 seconds for connection:
  uint8_t timeout = 10;
  while (timeout && (WiFi.status() != WL_CONNECTED)) {
    timeout--;
    delay(1000);
  }

  if (WiFi.status() == WL_CONNECTED) {
    lcd.setCursor(7, 1);
    lcd.print(F("Connected"));
    delay(1000);
    
    #ifdef DEBUG
      Serial.println(F("Connected!"));
      displayConnectionDetails();
    #endif
    return true;
  } else {
    lcd.setCursor(7, 1);
    lcd.print(F("Failed"));
    delay(1000);
    return false;
  }
}

/**************************************************************************/
/*!
    @brief  Tries to read the IP address and other connection details
*/
/**************************************************************************/
bool displayConnectionDetails(void) {
  #ifdef DEBUG
    // print the SSID of the network you're attached to:
    Serial.print("SSID: ");
    Serial.println(WiFi.SSID());
  
    // print your WiFi shield's IP address:
    IPAddress ip = WiFi.localIP();
    Serial.print("IP Address: ");
    Serial.println(ip);
  
    // print the received signal strength:
    long rssi = WiFi.RSSI();
    Serial.print("signal strength (RSSI):");
    Serial.print(rssi);
    Serial.println(" dBm");
  #endif
  return true;
}
