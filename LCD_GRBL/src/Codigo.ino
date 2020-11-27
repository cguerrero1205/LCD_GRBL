/*
  This code is based on the project found at "https://github.com/bvenneker/Arduino-GCode-Sender",
  by the user Bart Venneker. You can go to the link to see the original project.
  Thanks to him for sharing the code under a free license.
  Origanl Version 15-10-2017.002

  This code was updated by Carlos Guerrero.
  03/05/2020
  v1.0

  Hardware
  Arduino MEGA
  - Serial1 (pins 18 and 19) communicates with arduino with grbl shield

  GCode Sender.
  Hardware: Arduino Mega,
         ,rotary encoder
         ,sd card reader with SPI interface
         ,a general 4x20 LCD display with i2c interface
         ,a button for E-STOP

  Limitations: It does not support directories on the SD card, only files in the root directory are supported. Only support GRBL v1.1

  (Micro) SD card attached to SPI bus as follows:
  CS - pin 53
  MOSI - pin 51
  MISO - pin 50
  CLK - pin 52

  Rotary Enconder attached af follows:
  CLK - pin 2
  DT - pin 3
  SW - pin 4

  LCD Display
  the LCD display is connected to pins 20 (SDA) and 21 (SCL) (default i2c connections)

  Arduino pins:
  D18 rx1 : connects to tx on other arduino uno with GRBL 1.1
  D19 tx1 : connects to rx on other arduino uno with GRBL 1.1
  D20   : LCD Display (SDA) geel
  D21   : LCD Display (SCL) wit
  D53  : SD Card CS
  D51  : SD Card MOSI
  D50  : SD Card MISO
  D52  : SD Card Clock
  D2   : CLK
  D3   : DT
  D4   : SW

*/

#include <Arduino.h>
#include <EEPROM.h>
#include <LiquidCrystal_I2C.h> // by Frank de Brabander, available in the arduino library manager
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Encoder.h>  // by Paul Stoffregen, available in the arduino library manager

#define SD_card_Reader   53
#define clkPin            2
#define dtPin             3
#define selectPin         4

#define ENCODER_USE_INTERRUPTS

// Display
LiquidCrystal_I2C lcd(0x27, 20, 4); // set the LCD address to 0x27 for a 20 chars and 4 line display

//Encoder
Encoder myEnc(clkPin, dtPin);

// Globals
char WposX[9];            // last known X pos on workpiece, space for 9 characters ( -999.999\0 )
char WposY[9];            // last known Y pos on workpiece
char WposZ[9];            // last known Z heighton workpiece, space for 8 characters is enough( -99.999\0 )

char machineStatus[10];   // last know state (Idle, Run, Hold, Door, Home, Alarm, Check)

bool awaitingOK = false;   // this is set true when we are waiting for the ok signal from the grbl board (see the sendCodeLine() void)
bool homing = false, modWhileRun = false;

unsigned long runningTime, lastUpdateMenu, timeWithOutPress, timeExit = 5000, lastButtonCheck;

unsigned int numLineTotal = 0, numLineAct = 0;

long oldPosition  = 0;

byte timeDelay = 150, optionSelectLast;

void setup() {
  // display
  lcd.init();                      // initialize the lcd
  lcd.backlight();
  lcd.begin(20, 4);

  // inputs (write high to enable pullup)
  pinMode(clkPin, INPUT_PULLUP);
  pinMode(dtPin, INPUT_PULLUP);
  pinMode(selectPin, INPUT_PULLUP);

  // Ask to connect (you might still use a computer and NOT connect this controller)
  setTextDisplay(F("   Connect to CNC?"), "", F("    LCD GRBL v1.1"), F(" By Carlos Guerrero"));
  while (digitalRead(selectPin)) {}  // wait for the button to be pressed
  delay(50);
  while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
  delay(50);
  // Serial1 connections
  Serial.begin(115200);
  Serial.println(F("LCD GRBL, 1.0"));
  byte baudRate = EEPROM.read(0);
  switch (baudRate) {
    case 1:
      Serial1.begin(9600);
      break;
    case 2:
      Serial1.begin(19200);
      break;
    case 3:
      Serial1.begin(38400);
      break;
    case 4:
      Serial1.begin(57600);
      break;
    case 5:
      Serial1.begin(115200);
      break;
  }
  sendCodeLine(F("$10=0"), false); // Status report: Enable WPos and Disable MPos
  lcd.clear();
}

byte fileMenu() {
  /*
    This is the file menu.
    You can browse up and down to select a file.
    Click the button to select a file

    Move the stick right to exit the file menu and enter the Move menu
  */
  bool readySD = true;
  if (!SD.begin(SD_card_Reader)) {
    readySD = false;
    setTextDisplay(F("Error"), F("SD Card Fail!"), F(" "), F("=>Refresh"));
    timeWithOutPress = millis();

    while (millis() - timeWithOutPress <= timeExit) {
      if (digitalRead(selectPin) == LOW) {  // Pushed it!
        delay(50);
        while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
        delay(50);
        timeWithOutPress = millis();
        if (SD.begin(SD_card_Reader)) {
          readySD = true;
        }
      }
    }
  }
  if (readySD) {
    byte fileindex = 1;
    String fn;
    byte fc = filecount();

    fn = getFileName(fileindex);
    setTextDisplay(F("Files "), " -> " + (String)fn, " ", F("Click to select"));
    timeWithOutPress = millis();

    while (millis() - timeWithOutPress <= timeExit) {
      long newPosition = myEnc.read();
      byte diferencia = abs(newPosition - oldPosition);
      if (fileindex < fc && newPosition > oldPosition && diferencia != 3) { // down!
        fileindex++;
        timeWithOutPress = millis();
        fn = getFileName(fileindex);
        lcd.setCursor(0, 1);
        lcd.print(F(" -> ")); lcd.print(fn);
        for (int u = fn.length() + 4; u < 20; u++) {
          lcd.print(F(" "));
        }
      }

      if (newPosition < oldPosition && diferencia != 3) { // up!
        if (fileindex > 1) {
          fileindex--;
          timeWithOutPress = millis();
          fn = "";
          fn = getFileName(fileindex);
          lcd.setCursor(0, 1);
          lcd.print(F(" -> ")); lcd.print(fn);
          for (int u = fn.length() + 4; u < 20; u++) {
            lcd.print(F(" "));
          }
        }
      }
      if (fileindex > 0 && digitalRead(selectPin) == LOW && fn != "") { // Pushed it!
        setTextDisplay(F("Send this file? "), " -> " + fn, "", F("Click to confirm")); // Ask for confirmation
        delay(50);
        while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
        delay(50);

        unsigned long t = millis();
        while (millis() - t <= 2000UL) {
          if (digitalRead(selectPin) == LOW) { // Press the button again to confirm
            delay(10);
            while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
            return fileindex;
            break;
          }
        }
        timeWithOutPress = millis();
        setTextDisplay(F("Files "), " -> " + fn, "", F("Click to select"));
      }
      if (newPosition != oldPosition) {
        oldPosition = newPosition;
        delay(timeDelay);
      }
    }
  }
  return 0;
}

void moveMenu(char axis, float distance) {
  lcd.clear();
  String MoveCommand;
  String InitialCommand = "$J=G21G91";
  String SpeedCommand = "F1000";
  unsigned long lastUpdate;

  clearRXBuffer();
  float d = distance / 10;
  setTextDisplay("", "", "", "Move " + (String)axis + " " + (String)d + "mm");

  while (MoveCommand != "-1") {
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    MoveCommand = "";
    // read the state of all inputs

    if (newPosition > oldPosition && diferencia != 3) MoveCommand = InitialCommand + axis + distance / 10 + SpeedCommand;
    else if (newPosition < oldPosition && diferencia != 3) MoveCommand = InitialCommand + axis + (-distance / 10) + SpeedCommand;

    if (MoveCommand != "") {
      // send the commands
      sendCodeLine(MoveCommand, true);
      MoveCommand = "";
    }

    // get the status of the machine and monitor the receive buffer for OK signals
    if (millis() - lastUpdate >= 250) {
      getStatus();
      lastUpdate = millis();
      updateDisplayStatus(2);
    }

    if (digitalRead(selectPin) == LOW) { // button is pushed, exit the move loop
      lcd.clear();
      MoveCommand = F("-1");
      while (digitalRead(selectPin) == LOW) {}; // wait until the user releases the button
      delay(10);
    }
    if (newPosition != oldPosition) {
      oldPosition = newPosition;
      delay(timeDelay);
    }
  }
  sendCodeLine(F("G21"), true);
  sendCodeLine(F("G90"), true); // Switch to relative coordinates
}

String getFileName(byte i) {
  /*
    Returns a filename.
    if i = 1 it returns the first file
    if i = 2 it returns the second file name
    if i = 3 ... see what I did here?
  */
  byte x = 0;
  String result;
  File root = SD.open("/");
  while (result == "") {
    File entry =  root.openNextFile();
    if (!entry) {
      // noting
    } else {
      if (!entry.isDirectory()) {
        x++;
        if (x == i) result = entry.name();
      }
      entry.close();
    }
  }
  root.close();
  return result;
}

byte filecount() {
  /*
    Count the number of files on the SD card.
  */

  byte c = 0;
  File root = SD.open("/");
  while (true) {
    File entry =  root.openNextFile();
    if (! entry) {
      root.rewindDirectory();
      root.close();
      return c;
      break;
    } else  {
      if (!entry.isDirectory()) c++;
      entry.close();
    }
  }
}

void setTextDisplay(String line1, String line2, String line3, String line4) {
  /*
    This writes text to the display
  */
  if (line1 != "") {
    lcd.setCursor(0, 0);
    lcd.print(line1);
    for (int u = line1.length() ; u < 20; u++) {
      lcd.print(F(" "));
    }
  }
  if (line2 != "") {
    lcd.setCursor(0, 1);
    lcd.print(line2);
    for (int u = line2.length() ; u < 20; u++) {
      lcd.print(F(" "));
    }
  }
  if (line3 != "") {
    lcd.setCursor(0, 2);
    lcd.print(line3);
    for (int u = line3.length() ; u < 20; u++) {
      lcd.print(F(" "));
    }
  }
  if (line4 != "") {
    lcd.setCursor(0, 3);
    lcd.print(line4);
    for (int u = line4.length() ; u < 20; u++) {
      lcd.print(F(" "));
    }
  }
}

bool Exit = false;
byte varMod;
void sendFile(byte fileIndex) {
  /*
    This procedure sends the cgode to the grbl shield, line for line, waiting for the ok signal after each line

    It also queries the machine status every 500 milliseconds and writes some status information on the display
  */
  String strLine = "";

  File dataFile;

  unsigned long lastUpdate;

  String filename;
  varMod = 100;
  Serial1.write(144); //0x90 reset feed, set to 100%
  delay(50);
  Serial1.write(153); //0x91 reset spindle, set to 100%
  delay(50);
  filename = getFileName(fileIndex);
  dataFile = SD.open(filename);
  if (!dataFile) {
    setTextDisplay(F("File"), "", F("Error, file not found"), "");
    delay(1000); // show the error
    return;
  }

  setTextDisplay(F("Loading..."), " ", " ", filename);

  // Set the Work Position to zero
  sendCodeLine(F("G90"), true); // absolute coordinates
  sendCodeLine(F("G21"), true);
  //sendCodeLine(F("G92 X0 Y0 Z0"),true);  // set zero
  clearRXBuffer();

  // reset the timer
  runningTime = millis();

  // Read the file and count the total line

  while ( dataFile.available() ) {
    strLine = dataFile.readStringUntil('\n');
    strLine = ignoreUnsupportedCommands(strLine);
    if (strLine != "") numLineTotal++;
  }

  dataFile = SD.open(filename);
  // Read the file and send it to the machine
  while (dataFile.available()) {
    if (!awaitingOK) {
      // If we are not waiting for OK, send the next line
      strLine = dataFile.readStringUntil('\n');
      strLine = ignoreUnsupportedCommands(strLine);
      if (strLine != "") {
        sendCodeLine(strLine, true);  // sending it!
        numLineAct++;
      }
    }

    // get the status of the machine and monitor the receive buffer for OK signals
    if ((millis() - lastUpdate >= 250) && (!modWhileRun)) {
      lastUpdate = millis();
      float complete = (float(numLineAct) / float(numLineTotal)) * 100.0;
      complete = round(complete);
      lcd.setCursor(14, 0);
      lcd.print(F(" "));
      lcd.print(complete, 0) ; lcd.print(F("%"));
      updateDisplayStatus(runningTime);
      setTextDisplay("", "", "", filename);
    }
    if (modWhileRun) modMenu();
    else checkButtonSlect();
    if (Exit) break;
  }


  /*
    End of File!
    All Gcode lines have been send but the machine may still be processing them
    So we query the status until it goes Idle
  */
  if (!Exit) {
    while (strcmp (machineStatus, "Idle") != 0) {
      delay(250);
      getStatus();
      updateDisplayStatus(runningTime);
    }
    // Now it is done.
    lcd.setCursor(15, 0); lcd.print(F("100%"));
    setTextDisplay("", F("     Completed"), " ", "");
  }
  else setTextDisplay(" ", F("      Aborted!"), " ", " ");
  Exit = false;
  numLineTotal = 0, numLineAct = 0;
  while (digitalRead(selectPin) == HIGH) {} // Wait for the button to be pressed
  delay(50);
  while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
  delay(50);
  dataFile.close();
  resetSDReader();
}

void checkButtonSlect() {
  if (millis() - lastButtonCheck >= timeDelay) {
    if (digitalRead(selectPin) == LOW) { // Press the button
      delay(10);
      while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
      modWhileRun = true;
    }
    lastButtonCheck = millis();
  }
}

bool title1 = true, title2 = true, modFeed = false, modSpindle = false;
byte optionSelectMod = 1;
void modMenu() {
  String table[] = {"  Abort", "  Hold", "  Resume", "  Feed", "  Spindle"};
  bool resetVar = false;
  if (title1) {
    setTextDisplay(F("    Control Menu"), table[0], table[1], table[2]);
    moveOption(optionSelectMod);
    title1 = false;
  }
  if (millis() - lastButtonCheck >= timeDelay) {
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    if (!modFeed && !modSpindle) {
      if (newPosition > oldPosition && diferencia != 3) {  // Press the button
        lastButtonCheck = millis();
        if (optionSelectMod < 5) optionSelectMod++;
        if (optionSelectMod > 3) setTextDisplay("", table[optionSelectMod - 3], table[optionSelectMod - 2], table[optionSelectMod - 1]);
        moveOption(optionSelectMod);
      }
      else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
        lastButtonCheck = millis();
        if (optionSelectMod > 1) optionSelectMod--;
        if (optionSelectMod >= 3) setTextDisplay("", table[optionSelectMod - 3], table[optionSelectMod - 2], table[optionSelectMod - 1]);
        moveOption(optionSelectMod);
      }
      else if (digitalRead(selectPin) == LOW) { // Press the button
        delay(10);
        while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
        switch (optionSelectMod) {
          case 1:
            /*
              This stops the machine immediately (feed hold signal)
              And then sends the soft reset signal to clear the command buffer
            */
            Serial1.println(F("!"));  // feed hold
            delay(20);
            Serial1.write(24); // soft reset, clear command buffer
            delay(20);
            // clear the RX receive buffer
            clearRXBuffer();
            Exit = true;
            resetVar = true;
            break;
          case 2:
            Serial1.println(F("!"));
            resetVar = true;
            break;
          case 3:
            Serial1.println(F("~"));
            resetVar = true;
            break;
          case 4:
            modFeed = true;
            break;
          case 5:
            modSpindle = true;
            break;
        }
      }
    }
    else {
      if (modFeed) {
        if (title2) {
          setTextDisplay(F(" "), F("      Mod Feed"), F(" "), F(" "));
          title2 = false;
          lcd.setCursor(8, 2);
          lcd.print(varMod);  lcd.print(F("% "));
        }
      }
      else {
        if (title2) {
          setTextDisplay(F(" "), F("     Mod Spindle"), F(" "), F(" "));
          title2 = false;
          lcd.setCursor(8, 2);
          lcd.print(varMod);  lcd.print(F("% "));
        }
      }
      if (newPosition > oldPosition && diferencia != 3) {
        lastButtonCheck = millis();
        if (varMod < 200) varMod += 10;
        if (modFeed) Serial1.write(145); //0x91
        else Serial1.write(154); //0x9A
      }
      else if (newPosition < oldPosition && diferencia != 3) {
        lastButtonCheck = millis();
        if (varMod > 10) varMod -= 10;
        if (modFeed) Serial1.write(146); //0x91
        else Serial1.write(155); //0x9B
      }
      else if (digitalRead(selectPin) == LOW) {
        delay(10);
        while (digitalRead(selectPin) == LOW) {}
        resetVar = true;
      }
    }
    if (newPosition != oldPosition) {
      oldPosition = newPosition;
      if (modFeed || modSpindle) {
        lcd.setCursor(8, 2);
        lcd.print(varMod);  lcd.print(F("% "));
      }
    }
  }
  if (millis() - lastButtonCheck >= timeExit) resetVar = true;
  if (resetVar) {
    modWhileRun = false; modFeed = false; modSpindle = false;
    title1 = true; title2 = true;
    optionSelectMod = 1;
    lcd.clear();
  }
}

void updateDisplayStatus(unsigned long runtime) {
  /*
    I had some issues with updating the display while carving a file
    I created this extra void, just to update the display while carving.
  */

  unsigned long t = millis() - runtime;
  int H, M, S;
  char timeString[9];

  t = t / 1000;
  // Now t is the a number of seconds.. we must convert that to "hh:mm:ss"
  H = floor(t / 3600);
  t = t - (H * 3600);
  M = floor(t / 60);
  S = t - (M * 60);

  sprintf (timeString, "%02d:%02d:%02d", H, M, S);
  timeString[8] = '\0';
  getStatus();

  lcd.setCursor(0, 0);
  lcd.print(machineStatus);
  lcd.print(F(" "));
  if (runtime == 1) {
    lcd.setCursor(0, 3);
    lcd.print(F("Status Machine Wpos"));
  }
  else if (runtime > 3) {
    lcd.print(timeString);
    lcd.print(F("  "));
  }

  lcd.setCursor(0, 1);
  lcd.print(F("X:"));  lcd.print(WposX); lcd.print(F("  "));

  lcd.setCursor(11, 1);
  lcd.print(F("Y:"));  lcd.print(WposY);
  if (strlen(WposY) == 6)lcd.print(F(" "));
  else if (strlen(WposY) == 5) lcd.print(F("  "));

  lcd.setCursor(5, 2);
  lcd.print(F("Z:"));  lcd.print(WposZ); lcd.print(F("  "));
}

void resetSDReader() {
   while (!SD.begin(SD_card_Reader)) {
    setTextDisplay(F("Error"), F("SD Card Fail!"), "", "");
    delay(2000);
  }
}

void sendCodeLine(String lineOfCode, bool waitForOk ) {
  /*
    This void sends a line of code to the grbl shield, the grbl shield will respond with 'ok'
    but the response may take a while (depends on the command).
    So we immediately check for a response, if we get it, great!
    if not, we set the awaitingOK variable to true, this tells the sendfile() to stop sending code
    We continue to monitor the rx buffer for the 'ok' signal in the getStatus() procedure.
  */
  int updateScreen = 0 ;
  Serial.print("Send ");
  if ( waitForOk ) Serial.print(F("and wait, "));
  Serial.println(lineOfCode);

  Serial1.println(lineOfCode);
  awaitingOK = true;
  // delay(10);
  Serial.println(F("SendCodeLine calls for CheckForOk"));
  checkForOk();
  while (waitForOk && awaitingOK) {
    delay(50);
    // this may take long, so still update the timer on screen every second or so
    if ((updateScreen++ > 4) && (!modWhileRun)) {
      updateScreen = 0;
      updateDisplayStatus(runningTime);
    }
    if (modWhileRun) modMenu();
    else checkButtonSlect();
    checkForOk();
  }
}

void clearRXBuffer() {
  /*
    Just a small void to clear the RX buffer.
  */
  char vvv;
  while (Serial1.available()) {
    vvv = Serial1.read();
    delay(3);
  }
}

String ignoreUnsupportedCommands(String lineOfCode) {
  /*
    Remove unsupported codes, either because they are unsupported by GRBL.
  */
  removeIfExists(lineOfCode, F("G4"));
  removeIfExists(lineOfCode, F("G10 L2"));
  removeIfExists(lineOfCode, F("G10 l20"));
  removeIfExists(lineOfCode, F("G28"));
  removeIfExists(lineOfCode, F("G30"));
  removeIfExists(lineOfCode, F("G28.1"));
  removeIfExists(lineOfCode, F("G30.1"));
  removeIfExists(lineOfCode, F("G53"));
  removeIfExists(lineOfCode, F("G92"));
  removeIfExists(lineOfCode, F("G92.1"));

  // Ignore comment lines
  // Ignore tool commands, I do not support tool changers
  if (lineOfCode.startsWith("/") || lineOfCode.startsWith("T")) lineOfCode = "";
  lineOfCode.trim();
  return lineOfCode;
}

String removeIfExists(String lineOfCode, String toBeRemoved ) {
  if (lineOfCode.indexOf(toBeRemoved) >= 0 ) lineOfCode.replace(toBeRemoved, " ");
  return lineOfCode;
}

void checkForOk() {
  // read the receive buffer (if anything to read)
  char c, lastc;
  bool error5 = false;
  c = 64;
  lastc = 64;
  while (Serial1.available()) {
    c = Serial1.read();
    if (lastc == ':' && c == '5') error5 = true;
    else if (lastc == 'o' && c == 'k') {
      awaitingOK = false;
      Serial.println(F("< OK"));
    }
    lastc = c;
    delay(3);
  }
  if (error5) {
    Serial.println(F("(error:5) Homing cycle failure. Homing is not enabled via settings."));
    setTextDisplay(F("error:5"), F("Homing cycle failure"), F("It's not enabled"), F("via settings ($22)"));
    delay(5000);
  }
}

void getStatus() {
  /*
    This gets the status of the machine
    The status message of the machine might look something like this (this is a worst scenario message)
    The max length of the message is 72 characters long (including carriage return).
    <Idle|WPos:0.000,0.000,0.000|FS:0,0>
  */

  char content[80];
  char character;
  byte index = 0;
  bool completeMessage = false;
  int i = 0;
  int c = 0;
  Serial.println(F("GetStatus calls for CheckForOk"));
  checkForOk();

  Serial1.print(F("?"));  // Ask the machine status
  unsigned long times = millis();
  while (Serial1.available() == 0) {
    if (homing) setTextDisplay(F("       Homing"), F("       Cycle"), F(" "), F("   Please Wait..."));
    else if (millis() - times >= 10000) settingMenu();
  }  // Wait for response
  while (Serial1.available()) {
    character = Serial1.read();
    content[index] = character;
    if (content[index] == '>') completeMessage = true; // a simple check to see if the message is complete
    if (index > 0) {
      if (content[index] == 'k' && content[index - 1] == 'o') {
        awaitingOK = false;
        Serial.println(F("< OK from status"));
      }
    }
    index++;
    delay(1);
  }

  if (!completeMessage) {
    return;
  }
  Serial.println(content);
  i++;
  while (c < 9 && content[i] != '|') {
    machineStatus[c++] = content[i++];  // get the machine status
    machineStatus[c] = 0;
  }
  while (content[i++] != ':') ; // skip until the first ':'
  c = 0;
  while (c < 8 && content[i] != ',') {
    WposX[c++] = content[i++];  // get WposX
    WposX[c] = 0;
  }
  c = 0; i++;
  while (c < 8 && content[i] != ',') {
    WposY[c++] = content[i++];  // get WposY
    WposY[c] = 0;
  }
  c = 0; i++;
  while (c < 8 && content[i] != '|') {
    WposZ[c++] = content[i++];  // get WposZ
    WposZ[c] = 0;
  }
  if (WposZ[0] == '-')
  {
    WposZ[5] = '0';
    WposZ[6] = 0;
  }
  else
  {
    WposZ[4] = '0';
    WposZ[5] = 0;
  }
}

void menuP() {
  timeWithOutPress = millis();
  byte optionSelect = 1;
  setTextDisplay(F("    Main Screen"), F("  Control"), F("  Setting"), F("  Card Menu"));
  moveOption(optionSelect);
  while (millis() - timeWithOutPress <= timeExit) {
    long newPosition = myEnc.read();

    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect < 3) optionSelect++;
      moveOption(optionSelect);
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect > 1) optionSelect--;
      moveOption(optionSelect);
    }
    else if (digitalRead(selectPin) == LOW) { // Press the button
      delay(10);
      while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1:
          controlMenu();
          break;
        case 2:
          settingMenu();
          break;
        case 3:
          byte a = fileMenu();
          if (a != 0) sendFile(a);
          break;
      }
      break;
    }
    if (newPosition != oldPosition) {
      oldPosition = newPosition;
      delay(timeDelay);
    }
  }
  lcd.clear();
}

void controlMenu() {
  String table[] = {"  Unlock GRBL", "  Auto Home", "  Move Axis", "  Reset Zero", "  Return to Zero", "  Spindle Speed"};
  timeWithOutPress = millis();
  byte optionSelect = 1;
  setTextDisplay(F("    Control Menu"), table[0], table[1], table[2]);
  moveOption(optionSelect);
  while (millis() - timeWithOutPress <= timeExit) {
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect < 6) optionSelect++;
      if (optionSelect > 3) setTextDisplay("", table[optionSelect - 3], table[optionSelect - 2], table[optionSelect - 1]);
      moveOption(optionSelect);
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect > 1) optionSelect--;
      if (optionSelect >= 3) setTextDisplay("", table[optionSelect - 3], table[optionSelect - 2], table[optionSelect - 1]);
      moveOption(optionSelect);
    }
    else if (digitalRead(selectPin) == LOW) { // Press the button
      delay(10);
      while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1:
          sendCodeLine(F("$X"), true);
          break;
        case 2:
          homing = true;
          sendCodeLine(F("$H"), true);
          sendCodeLine(F("G10 P0 L20 X0 Y0 Z0"), true);
          homing = false;
          break;
        case 3:
          menuMoveAxis();
          break;
        case 4:
          sendCodeLine(F("G10 P0 L20 X0 Y0 Z0"), true);
          break;
        case 5:
          sendCodeLine(F("G90 G0 X0 Y0"), true);
          sendCodeLine(F("G90 G0 Z0"), true);
          break;
        case 6:
          setTextDisplay(F(" "), F("     Coming"), F("      Soon"), F(" "));
          break;
      }
      break;
    }
    if (newPosition != oldPosition) {
      oldPosition = newPosition;
      delay(timeDelay);
    }
  }
  lcd.clear();
}

void menuMoveAxis() {
  timeWithOutPress = millis();
  byte optionSelect = 1;
  setTextDisplay(F("   Main Move Axis"), F("  Move 10.000mm"), F("  Move  1.000mm"), F("  Move  0.100mm"));
  moveOption(optionSelect);
  while (millis() - timeWithOutPress <= timeExit) {
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect < 3) optionSelect++;
      moveOption(optionSelect);
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect > 1) optionSelect--;
      moveOption(optionSelect);
    }
    else if (digitalRead(selectPin) == LOW) { // Press the button
      delay(10);
      while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1:
          setAxisToMove(100);
          break;
        case 2:
          setAxisToMove(10);
          break;
        case 3:
          setAxisToMove(1);
          break;
      }
      break;
    }
    if (newPosition != oldPosition) {
      oldPosition = newPosition;
      delay(timeDelay);
    }
  }
  lcd.clear();
}

void setAxisToMove(byte distance) {
  timeWithOutPress = millis();
  byte optionSelect = 1;
  float d = float(distance / 10.0);
  setTextDisplay("    Move " + (String)d + " mm", F("  Move X"), F("  Move Y"), F("  Move Z"));
  moveOption(optionSelect);
  while (millis() - timeWithOutPress <= timeExit) {
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect < 3) optionSelect++;
      moveOption(optionSelect);
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect > 1) optionSelect--;
      moveOption(optionSelect);
    }
    else if (digitalRead(selectPin) == LOW) { // Press the button
      delay(10);
      while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1:
          moveMenu('X', distance);
          break;
        case 2:
          moveMenu('Y', distance);
          break;
        case 3:
          moveMenu('Z', distance);
          break;
      }
      break;
    }
    if (newPosition != oldPosition) {
      oldPosition = newPosition;
      delay(timeDelay);
    }
  }
  lcd.clear();
}

void settingMenu() {
  String table[] = {"  9600", "  19200", "  38400", "  57600", "  115200"};
  timeWithOutPress = millis();
  byte optionSelect = 1;

  setTextDisplay(F("   Baud Rate Menu"), table[0], table[1], table[2]);
  moveOption(optionSelect);

  while (millis() - timeWithOutPress <= timeExit) {
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect < 5) optionSelect++;
      if (optionSelect > 3) setTextDisplay("", table[optionSelect - 3], table[optionSelect - 2], table[optionSelect - 1]);
      moveOption(optionSelect);
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if (optionSelect > 1) optionSelect--;
      if (optionSelect >= 3) setTextDisplay("", table[optionSelect - 3], table[optionSelect - 2], table[optionSelect - 1]);
      moveOption(optionSelect);
    }
    else if (digitalRead(selectPin) == LOW) { // Press the button
      delay(10);
      while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1:
          EEPROM.update(0, 1);
          break;
        case 2:
          EEPROM.update(0, 2);
          break;
        case 3:
          EEPROM.update(0, 3);
          break;
        case 4:
          EEPROM.update(0, 4);
          break;
        case 5:
          EEPROM.update(0, 5);
          break;
      }
      asm("jmp 0x0000");
    }
    if (newPosition != oldPosition) {
      oldPosition = newPosition;
      delay(timeDelay);
    }
  }
  lcd.clear();
}

void moveOption(byte optionSelect) {
  if (optionSelect > 3) optionSelect = 3;
  lcd.setCursor(0, optionSelectLast); lcd.print(F("  "));
  lcd.setCursor(0, optionSelect);
  lcd.print(F("=>"));
  optionSelectLast = optionSelect;
}

void loop() {
  if (millis() - lastUpdateMenu >= 250) {
    lastUpdateMenu = millis();
    updateDisplayStatus(1);
  }
  if (digitalRead(selectPin) == LOW) { // Press the button
    delay(10);
    while (digitalRead(selectPin) == LOW) {} // Wait for the button to be released
    menuP();
  }
}
