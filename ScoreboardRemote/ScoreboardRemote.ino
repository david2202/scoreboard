/*
 * Sketch for a cricket scoreboard.  Requires the following hardware:
 * - 1602 LCD display
 * - 7 pushbuttons
 * - NRF24L01 2.4 GHz transceiver
 * - CC3000 Wifi breakout
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
#include <EEPROM.h>
#include <EEPROMAnything.h>
#include <Buttons.h>
#include <ScoreboardCommon.h>
#include <Adafruit_CC3000.h>
#include <ccspi.h>
#include <SPI.h>
#include "nRF24L01.h"
#include "RF24.h"
#include <string.h>

//#define DEBUG

// These are the interrupt and control pins
#define ADAFRUIT_CC3000_IRQ   3  // MUST be an interrupt pin!
// These can be any two pins
// Mega
#define ADAFRUIT_CC3000_VBAT  47
#define ADAFRUIT_CC3000_CS    48

// Use hardware SPI for the remaining pins
// On an UNO, SCK = 13, MISO = 12, and MOSI = 11
// On a Mega, SCK = 52, MISO = 50, and MOSI = 51
Adafruit_CC3000 cc3000 = Adafruit_CC3000(ADAFRUIT_CC3000_CS, ADAFRUIT_CC3000_IRQ, ADAFRUIT_CC3000_VBAT,
                                         SPI_CLOCK_DIV4); // you can change this clock speed

#define WLAN_SSID           "XT1068 4797"           // cannot be longer than 32 characters!
//#define WLAN_SSID             "NETGEAR26"             // cannot be longer than 32 characters!
//#define WLAN_SSID           "NETGEAR-Guest"         // cannot be longer than 32 characters!
#define WLAN_PASS             "rockycurtain281"
#define WLAN_CONNECT_RETRIES  3

// Security can be WLAN_SEC_UNSEC, WLAN_SEC_WEP, WLAN_SEC_WPA or WLAN_SEC_WPA2
#define WLAN_SECURITY   WLAN_SEC_WPA2

#define IDLE_TIMEOUT_MS  3000      // Amount of time to wait (in milliseconds) with no data 
                                   // received before closing the connection.  If you know the server
                                   // you're accessing is quick to respond, you can reduce this value.
#define SEND_TIME_MS     30000

// ELB address
#define WEBSITE      "ec2-52-27-239-27.us-west-2.compute.amazonaws.com"
#define PORT         80

// Constants
const byte NBR_OF_BUTTONS = 7;                   // Number of buttons pins
const int MODE_TIME = 3000;                      // Time (in millis) that plus mode lasts
const int DEBOUNCE_DELAY = 40;                   // Delay time for debouncing buttons
const int REPEAT_INITIAL_DELAY = 1000;           // How long button must be held down before it starts repeating
const int REPEAT_DELAY = 100;                    // How long between repeats

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

//SPISettings settingsCC3000(SPI_CLOCK_DIV4, MSBFIRST, SPI_MODE1); 
//SPISettings settingsRF24(SPI_CLOCK_DIV4, MSBFIRST, SPI_MODE0); 

long modeOnTime = 0;                            // The time plus mode was turned on
byte opMode = SCOREBOARD_OPMODE;
int currentMode = NO_MODE;                      // NO_MODE, PLUS_MODE or MINUS_MODE
boolean modeFirstPress = true;                  // Is it the first button press on entering the mode?
boolean scoreDirty = false;                     // Does the score need to be sent to the remote?
score_t score;

// 192.168.0.98
//uint32_t ip = 3232235618;
uint32_t ip = 0;
unsigned long sendTime = 0;
Adafruit_CC3000_Client www;

void setup() {
  Serial.begin(115200);
  lcd.begin(16, 2);                             // Set the display to 16 columns and two rows
  lcd.clear();
  
  EEPROM_readAnything(0, score);                // Read the saved score from EEPROM

  initialiseRadio();
  // initialiseWiFi();

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
      // sendScoreWiFi();
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
  //SPI.beginTransaction(settingsRF24);
  radio.stopListening();
  bool ok = radio.write(&score, sizeof(score));
  #ifdef DEBUG
    Serial.println(ok);
  #endif
  radio.startListening();
  //SPI.endTransaction();
}

void sendScoreWiFi() {
//  SPI.beginTransaction(settingsCC3000);
  connectTCP();
  if (www.connected()) {
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

    www.fastrprintln(F("POST /score HTTP/1.1"));
    www.fastrprint(F("Host: ")); www.fastrprint(WEBSITE); www.fastrprint(":"); www.fastrprintln(String(PORT).c_str());
    www.fastrprintln(F("Connection: keep-alive"));
    www.fastrprint(F("Content-Length: ")); www.fastrprintln(String(payload.length()).c_str());
    www.fastrprintln(F("Cache-Control: no-cache"));
    www.fastrprintln(F("Content-Type: application/json"));
    www.fastrprintln(F("Accept: */*"));
    www.println();
    www.fastrprint(payload.c_str());
    www.println();
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
  //SPI.endTransaction();
}

void receiveResponseWiFi() {
//  SPI.beginTransaction(settingsCC3000);
    // Read any data that is available
  while (www.connected() && www.available()) {
    char c = www.read();
    #ifdef DEBUG
      Serial.print(c);
    #endif
  }

  // Give up waiting and close connection
  if (www.connected() && millis() > sendTime + IDLE_TIMEOUT_MS) {
    #ifdef DEBUG
      Serial.println(F("-------------------------------------"));
    #endif
    disconnectTCP();
  }
  //SPI.endTransaction();
}

void connectTCP() {
  if (!www.connected()) {
    www = cc3000.connectTCP(ip, PORT);
  }
}

void disconnectTCP() {
  www.close();
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
    EEPROM_writeAnything(0, score);                  // Store the current score in EEPROM
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
  EEPROM_writeAnything(0, score);
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
  EEPROM_writeAnything(0, score);
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
  //SPI.beginTransaction(settingsRF24);
  radio.openWritingPipe(PIPES[0]);
  lcd.setCursor(7, 0);
  lcd.print(F("Writing "));
  radio.openReadingPipe(1,PIPES[1]);
  lcd.setCursor(7, 0);
  lcd.print(F("Reading "));

  radio.startListening();
  lcd.setCursor(7, 0);
  lcd.print(F("Complete"));
  //SPI.endTransaction();
}

void initialiseWiFi() {
//  SPI.beginTransaction(settingsCC3000);
  lcd.setCursor(0, 1);
  lcd.print(F("WiFi:  Starting"));
  #ifdef DEBUG
    Serial.println(F("Hello, CC3000!"));
    Serial.println(F("Initializing..."));
  #endif
  
  if (!cc3000.begin()) {
    lcd.setCursor(7, 1);
    lcd.print(F("Failed  "));
    #ifdef DEBUG
      Serial.println(F("Couldn't begin()! Check your wiring?"));
    #endif
    delay(1000);
    return;
  }
//  cc3000.reboot();
//  cc3000.deleteProfiles();
  cc3000.setDHCP();
  lcd.setCursor(7, 1);
  lcd.print(WLAN_SSID);

  #ifdef DEBUG
    Serial.print(F("Attempting to connect to ")); Serial.println(WLAN_SSID);
  #endif
  if (!cc3000.connectToAP(WLAN_SSID, WLAN_PASS, WLAN_SECURITY, WLAN_CONNECT_RETRIES)) {
    lcd.setCursor(7, 1);
    lcd.print(F("Failed connect"));
    #ifdef DEBUG
      Serial.println(F("Failed!"));
    #endif
    delay(1000);
    return;
  }

  lcd.setCursor(7, 1);
  lcd.print(F("DHCP  "));

  #ifdef DEBUG
    Serial.println(F("Connected!"));
    Serial.println(F("Request DHCP"));
  #endif
  
  int i = 0;
  while (!cc3000.checkDHCP()) {
    lcd.setCursor(7, 1);
    lcd.print(F("DHCP #"));
    lcd.print(i);
    lcd.print(F("  "));
    #ifdef DEBUG
      Serial.print(F("DHCP failed #"));
      Serial.println(i);
    #endif
    if (i++ > 10) {
      #ifdef DEBUG
        Serial.println(F("Failed to get IP address!"));
      #endif
      return;
    }
    delay(1000);
  }

  i = 0;
  while (!displayConnectionDetails()) {
    #ifdef DEBUG
      Serial.print(F("Display connection details failed #"));
      Serial.println(i);
    #endif
    if (i++ > 10) {
      #ifdef DEBUG
        Serial.println(F("Failed to get IP address!"));
      #endif
      return;
    }
    delay(1000);
  }

  if (ip != 0) {
    #ifdef DEBUG
      Serial.println(F("Skipping DNS lookup - hardcoded IP"));
    #endif
  } else {
    lcd.setCursor(7, 1);
    lcd.print(F("DNS   "));

    #ifdef DEBUG
      Serial.print(F("Attempting DNS lookup for "));
      Serial.println(WEBSITE);
    #endif
    i = 0;
    while (!cc3000.getHostByName(WEBSITE, &ip)) {
      lcd.setCursor(7, 1);
      lcd.print(F("DNS #"));
      lcd.print(i);
      #ifdef DEBUG
        Serial.print(F("Resolve attempt failed #"));
        Serial.println(i);
      #endif
      if (i++ > 10) {
        #ifdef DEBUG
          Serial.println(F("DNS failed to resolve host"));
        #endif
        return;
      }
      delay(100);
    }
  }
  lcd.setCursor(7, 1);
  lcd.print(F("Complete"));
  delay(1000);
  #ifdef DEBUG
    Serial.print(F("Resolved host to IP address: "));
    cc3000.printIPdotsRev(ip);
    Serial.println();
    Serial.println(F("CC3000 initialisation complete"));
  #endif
  //SPI.endTransaction();
}

/**************************************************************************/
/*!
    @brief  Tries to read the IP address and other connection details
*/
/**************************************************************************/
bool displayConnectionDetails(void) {
  uint32_t ipAddress, netmask, gateway, dhcpserv, dnsserv;
  
  if(!cc3000.getIPAddress(&ipAddress, &netmask, &gateway, &dhcpserv, &dnsserv)) {
    return false;
  } else {
    #ifdef DEBUG
      Serial.print(F("\nIP Addr: ")); cc3000.printIPdotsRev(ipAddress);
      Serial.print(F("\nNetmask: ")); cc3000.printIPdotsRev(netmask);
      Serial.print(F("\nGateway: ")); cc3000.printIPdotsRev(gateway);
      Serial.print(F("\nDHCPsrv: ")); cc3000.printIPdotsRev(dhcpserv);
      Serial.print(F("\nDNSserv: ")); cc3000.printIPdotsRev(dnsserv);
      Serial.println();
    #endif
    return true;
  }
}
