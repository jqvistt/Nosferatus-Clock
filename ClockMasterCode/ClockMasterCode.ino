#include <Arduino.h>
#include <RTClib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Servo.h>
#define _TASK_MICRO_RES
#include <TaskScheduler.h>
#include <SafeString.h>

Scheduler runner;  // Create the scheduler

// Task declaration
const unsigned long taskInterval = 500;  //TASK INTERVAL (microseconds)

void calibrateSkullTaskCallback();
Task calibrateSkullTask(taskInterval, TASK_FOREVER, &calibrateSkullTaskCallback);

void skullTaskCallback();
Task skullTask(taskInterval, TASK_FOREVER, &skullTaskCallback);

void calibrateReaperTaskCallback();
Task calibrateReaperTask(taskInterval, TASK_FOREVER, &calibrateReaperTaskCallback);

void reaperTaskCallback();
Task reaperTask(taskInterval, TASK_FOREVER, &reaperTaskCallback);

void strikeBellTaskCallback();
Task strikeBellTask(taskInterval, TASK_FOREVER, &strikeBellTaskCallback);

RTC_DS3231 rtc;

// Constants and Pin Definitions
#define MOTOR_STEPS 2048

#define STEP_REAPER 3
#define DIR_REAPER 4
#define ENABLE_REAPER 2
const int LIMIT_REAPER = A1;

#define STEP_SKULL 6
#define DIR_SKULL 7
#define ENABLE_SKULL 5
const int LIMIT_SKULL = A2;

//SERVO SHENANIGANS
#define DOOR_RIGHT 8
Servo doorRight;
#define DOOR_LEFT 12
Servo doorLeft;

int closedServoPos = 70;

#define SOLENOID_SKULL 11
#define SOLENOID_BELL 10

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int keyPin = A0;
int analogInputValue = 0;
int roundedInputValue = 0;
String inputValue = "";

unsigned long lastButtonPress = 0;
unsigned long lastInputTime = 0;
unsigned long lastDisplayUpdate = 0;

bool isSettingTime = false;
int settingIndex = 0;  // 0: hours, 1: minutes, 2: seconds

// Declare the time variables globally
int hours = 0;
int minutes = 0;
int seconds = 0;

const unsigned long DEBOUNCE_DELAY_INIT = 500;
const unsigned long DEBOUNCE_DELAY_HOLD = 100;

unsigned long queuedStrikes = 0;



void SetPinModes() {
  pinMode(keyPin, INPUT);

  pinMode(ENABLE_REAPER, OUTPUT);
  pinMode(DIR_REAPER, OUTPUT);
  pinMode(STEP_REAPER, OUTPUT);
  pinMode(LIMIT_REAPER, INPUT_PULLUP);

  pinMode(ENABLE_SKULL, OUTPUT);
  pinMode(DIR_SKULL, OUTPUT);
  pinMode(STEP_SKULL, OUTPUT);
  pinMode(LIMIT_SKULL, INPUT_PULLUP);

  pinMode(SOLENOID_SKULL, OUTPUT);
  pinMode(SOLENOID_BELL, OUTPUT);

  pinMode(DOOR_RIGHT, OUTPUT);
  pinMode(DOOR_LEFT, OUTPUT);
}

void InitializeDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed"));
    for (;;)
      ;
  }
  display.clearDisplay();
  delay(1000);  //small debounce
  display.display();
}

void InitializeSteppers() {
  digitalWrite(ENABLE_REAPER, HIGH);  // Disable reaper stepper
  digitalWrite(ENABLE_SKULL, HIGH);   // Disable skull stepper

  digitalWrite(DIR_REAPER, HIGH);
}

void InitializeServos() {
  doorLeft.attach(DOOR_LEFT);
  doorRight.attach(DOOR_RIGHT);

  doorLeft.write(180 - closedServoPos);  // Initial position
  doorRight.write(closedServoPos);       // Initial position
}

void InitializeRTC() {
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1)
      ;
  }
  if (rtc.lostPower()) {
    Serial.println("RTC lost power, setting the time!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

bool isEventActive() {  // bool that mainly makes sure that no display updates take place when an event is active.
  if (skullTask.isEnabled() || reaperTask.isEnabled() || calibrateSkullTask.isEnabled() || calibrateReaperTask.isEnabled() || strikeBellTask.isEnabled()) {
    return true;
  } else {
    return false;
  }
}

String checkKeyInput() {
  analogInputValue = analogRead(keyPin);

  // Round the analog input value to the nearest 10
  roundedInputValue = round(analogInputValue / 10.0) * 10;

  if (roundedInputValue == 350) {
    return "set";
  } else if (roundedInputValue == 170) {
    return "down";
  } else if (roundedInputValue == 0) {
    return "up";
  } else if (roundedInputValue == 90) {
    return "left";
  } else if (roundedInputValue == 30) {
    return "right";
  } else {
    return "";
  }
}

void handleInput(String inputValue) {
  static unsigned long lastPressTime = 0;
  static unsigned long debounceDelay = DEBOUNCE_DELAY_INIT;
  static bool isHolding = false;
  static String lastInputValue = "";

  unsigned long currentTime = millis();

  if (inputValue != "") {
    if (inputValue != lastInputValue) {
      // New button press
      lastInputValue = inputValue;
      isHolding = false;
      debounceDelay = DEBOUNCE_DELAY_INIT;
      lastPressTime = currentTime;
      processInput(inputValue);
    } else {
      // Button is being held down
      if (!isHolding) {
        if (currentTime - lastPressTime >= debounceDelay) {
          isHolding = true;
          debounceDelay = DEBOUNCE_DELAY_HOLD;  // Reduce delay for holding
          lastPressTime = currentTime;
          processInput(inputValue);
        }
      } else {
        // Holding with reduced debounce
        if (currentTime - lastPressTime >= debounceDelay) {
          lastPressTime = currentTime;
          processInput(inputValue);
        }
      }
    }
  } else {
    // No button pressed
    lastInputValue = "";
    isHolding = false;
  }
}

void processInput(String inputValue) {
  if (inputValue == "set") {
    if (!isSettingTime) {
      DateTime now = rtc.now();
      hours = now.hour();
      minutes = now.minute();
      seconds = now.second();
    } else {
      setRTCTime();
    }
    isSettingTime = !isSettingTime;
    settingIndex = 0;
  }

  if (isSettingTime) {
    if (inputValue == "left") {
      settingIndex = (settingIndex + 2) % 3;  // Move left in the setting index
    } else if (inputValue == "right") {
      settingIndex = (settingIndex + 1) % 3;  // Move right in the setting index
    } else if (inputValue == "up") {
      increaseTime();
    } else if (inputValue == "down") {
      decreaseTime();
    }
  }
}

void increaseTime() {
  if (settingIndex == 0) {
    hours = (hours + 1) % 24;
  } else if (settingIndex == 1) {
    minutes = (minutes + 1) % 60;
  } else if (settingIndex == 2) {
    seconds = (seconds + 1) % 60;
  }
}

void decreaseTime() {
  if (settingIndex == 0) {
    hours = (hours + 23) % 24;
  } else if (settingIndex == 1) {
    minutes = (minutes + 59) % 60;
  } else if (settingIndex == 2) {
    seconds = (seconds + 59) % 60;
  }
}

void setRTCTime() {
  rtc.adjust(DateTime(2021, 1, 1, hours, minutes, seconds));
}

void displayTime() {

  if (!isEventActive()) {  //dont do jack shit with the display if event is running!

    DateTime now = rtc.now();
    if (!isSettingTime) {
      hours = now.hour();
      minutes = now.minute();
      seconds = now.second();
    }

    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);

    display.print(hours < 10 ? "0" : "");
    display.print(hours);
    display.print(":");
    display.print(minutes < 10 ? "0" : "");
    display.print(minutes);
    display.print(":");
    display.print(seconds < 10 ? "0" : "");
    display.print(seconds);

    if (isSettingTime) {
      display.setTextSize(2);
      display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
      if (settingIndex == 0) {
        display.setCursor(0, 0);
        display.print(hours < 10 ? "0" : "");
        display.print(hours);
      } else if (settingIndex == 1) {
        display.setCursor(35, 0);
        display.print(minutes < 10 ? "0" : "");
        display.print(minutes);
      } else {
        display.setCursor(70, 0);
        display.print(seconds < 10 ? "0" : "");
        display.print(seconds);
      }

      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 20);
      display.print("Setting Time...");
    }
    display.display();
  } else {
    display.clearDisplay();
  }
}

void EventHandler() {
  DateTime now = rtc.now();

  if (now.hour() == 0 && now.minute() == 0 && now.second() == 0 && !isSettingTime) {
    queuedStrikes = 12;
    skullTask.restart();
    reaperTask.restartDelayed(10000000);
  } else if (now.minute() == 30 && now.second() == 0 && !isSettingTime) {
    queuedStrikes = 1;
    strikeBellTask.restart();
  } else if (now.hour() != 0 && now.minute() == 0 && now.second() == 0 && !isSettingTime) {
    if (now.hour() > 12) {
      queuedStrikes = now.hour() - 12;
    } else {
      queuedStrikes = now.hour();
    }
    skullTask.restart();
  }
}

// TASKS

void calibrateSkullTaskCallback() {
  static int state = 0;
  static unsigned long previousMillis = 0;
  unsigned long currentMillis = millis();
  const unsigned long stepDelay = 2;  // move slowly so the elevator has time to reach the limit switch

  switch (state) {

    case 0:
      Serial.println("calibrateSkullTask | State 0: Enabling SKULL STEPPER for calibration!");
      digitalWrite(ENABLE_SKULL, LOW);  // ENABLE SKULL STEPPER
      state = 1;
      break;

    case 1:
      if (digitalRead(LIMIT_SKULL) == LOW) {
        Serial.println("calibrateSkullTask | LIMIT SWITCH REACHED, MOVING ONTO STEP 3");
        state = 3;
      } else if (currentMillis - previousMillis >= stepDelay) {
        digitalWrite(STEP_SKULL, HIGH);
        previousMillis = currentMillis;
        state = 2;
      }
      break;

    case 2:
      if (currentMillis - previousMillis >= stepDelay) {
        digitalWrite(STEP_SKULL, LOW);
        previousMillis = currentMillis;
        state = 1;
      }
      break;

    case 3:
      Serial.println("calibrateSkullTask | State 3: Calibration complete, disabling SKULL STEPPER");
      digitalWrite(ENABLE_SKULL, HIGH);  // DISABLE SKULL STEPPER
      //skullTask.restart();               // START THE ACTUAL SKULL TASK
      calibrateSkullTask.disable();  // Disable the task as it is complete
      state = 0;                     // Reset the state machine
      break;
  }
}

void skullTaskCallback() {
  static int state = 0;
  static unsigned long stepCount = 0;
  static unsigned long strikeCount = 0;
  static unsigned long previousMillis = 0;

  static unsigned long stepOffset = 190;

  unsigned long currentMillis = millis();
  const unsigned long stepDelay = 1;  // Delay in milliseconds for stepping

  const unsigned long slowStepDelay = 2;  //delay for slow-stepping!

  const unsigned long delayBeforeStrike = 1000;       // Delay in microseconds before strike (1000 ms / 1s)
  const unsigned long delayAfterStrike = 500;         // Delay in microseconds after strike (500 ms / 0.5s)
  const unsigned long delayAfterStrikingState = 1000;  //Delay before moving on from the striking state (1000ms / 1s)

  switch (state) {
    case 0:
      Serial.println("skullTask | State 0: Enabling SKULL STEPPER");
      digitalWrite(ENABLE_SKULL, LOW);  // ENABLE SKULL STEPPER
      state = 1;
      break;

    case 1:
      if (stepCount >= 12 * (MOTOR_STEPS / 14) + stepOffset) {
        Serial.println("skullTask | State 1: Step count fulfilled, moving to state 3");
        state = 3;  // Swap to case 3 if the steps have been fulfilled (6/7ths of a rotation)
      } else if (currentMillis - previousMillis >= stepDelay) {
        //Serial.println("State 1: Stepping SKULL STEPPER HIGH");
        digitalWrite(STEP_SKULL, HIGH);
        previousMillis = currentMillis;
        state = 2;
      }
      break;

    case 2:
      if (currentMillis - previousMillis >= stepDelay) {
        //Serial.println("State 2: Stepping SKULL STEPPER LOW");
        digitalWrite(STEP_SKULL, LOW);
        stepCount++;
        //Serial.print("Step count: ");
        //Serial.println(stepCount);
        previousMillis = currentMillis;
        state = 1;
      }
      break;

    case 3:
      if (strikeCount >= queuedStrikes) {
        if (currentMillis - previousMillis >= delayAfterStrikingState) {
          Serial.println("skullTask | State 3: Strikes complete, moving to state 5");
          state = 5;  // If strikes are complete jump straight to case 5
          previousMillis = currentMillis;
        }
      } else if (currentMillis - previousMillis >= delayBeforeStrike) {
        Serial.println("skullTask | State 3: Activating solenoids for strike");
        digitalWrite(SOLENOID_SKULL, HIGH);
        digitalWrite(SOLENOID_BELL, HIGH);
        previousMillis = currentMillis;
        state = 4;
      }
      break;

    case 4:
      if (currentMillis - previousMillis >= delayAfterStrike) {
        Serial.println("skullTask | State 4: Deactivating solenoids after strike");
        digitalWrite(SOLENOID_SKULL, LOW);
        digitalWrite(SOLENOID_BELL, LOW);
        strikeCount++;
        Serial.print("skullTask | Strike count: ");
        Serial.println(strikeCount);
        previousMillis = currentMillis;
        state = 3;
      }
      break;

    case 5:
      if (digitalRead(LIMIT_SKULL) == LOW) {
        Serial.println("skullTask | State 5: LIMIT SWITCH REACHED, moving to state 7");
        state = 7;  // Swap to case 7 if the steps have been fulfilled (7/7ths of a rotation)
      } else if (currentMillis - previousMillis >= slowStepDelay) {
        digitalWrite(STEP_SKULL, HIGH);
        previousMillis = currentMillis;
        state = 6;
      }
      break;

    case 6:
      if (currentMillis - previousMillis >= slowStepDelay) {
        digitalWrite(STEP_SKULL, LOW);
        previousMillis = currentMillis;
        state = 5;
      }
      break;

    case 7:
      Serial.println("skullTask | State 7: Task complete, disabling SKULL STEPPER");
      // Disable stepper and solenoids as the task is complete
      digitalWrite(ENABLE_SKULL, HIGH);  // DISABLE SKULL STEPPER
      skullTask.disable();               // Disable the task as it is complete
      state = 0;                         // Reset the state machine
      stepCount = 0;                     // Reset step count for the next activation
      strikeCount = 0;                   // Reset strike count for the next activation
      break;
  }
}

void calibrateReaperTaskCallback() {
  static int state = 0;
  static unsigned long previousMillis = 0;
  static unsigned long stepCount = 0;
  unsigned long currentMillis = millis();
  const unsigned long stepDelay = 3;
  const unsigned long limitSwitchTimeout = 2000;

  switch (state) {
    case 0:
      Serial.println("calibrateReaperTask | State 0: Moving servos to open position");
      doorRight.write(0);
      doorLeft.write(180);
      previousMillis = currentMillis;
      state = 1;
      break;

    case 1:
      if (doorRight.read() <= 0 && doorLeft.read() >= 179) {
        Serial.println("calibrateReaperTask | State 1: Doors open, moving to state 2");
        state = 2;
      }
      break;

    case 2:
      if (digitalRead(LIMIT_REAPER) == LOW) {
        Serial.println("calibrateReaperTask | State 2: Reaper already positioned. Skipping calibration.");
        state = 5; // Move to state 5 to close doors
      } else {
        Serial.println("calibrateReaperTask | State 2: Enabling REAPER STEPPER");
        digitalWrite(ENABLE_REAPER, LOW);  // ENABLE REAPER STEPPER
        previousMillis = currentMillis;
        stepCount = 0;
        state = 3;
      }
      break;

    case 3:
      if (digitalRead(LIMIT_REAPER) == LOW) {
        Serial.println("calibrateReaperTask | State 3: Limit switch activated, stopping calibration");
        state = 5; // Move to state 5 to close doors
      } else if (currentMillis - previousMillis >= stepDelay) {
        digitalWrite(STEP_REAPER, HIGH);
        previousMillis = currentMillis;
        state = 4;
      }
      break;

    case 4:
      if (currentMillis - previousMillis >= stepDelay) {
        digitalWrite(STEP_REAPER, LOW);
        stepCount++;
        previousMillis = currentMillis;
        state = 3;
      }
      break;

    case 5:
      Serial.println("calibrateReaperTask | State 5: Moving servos to closed position");
      doorRight.write(closedServoPos);
      doorLeft.write(180 - closedServoPos);
      previousMillis = currentMillis;
      state = 6;
      break;

    case 6:
      if (doorRight.read() >= closedServoPos && doorLeft.read() <= (180 - closedServoPos)) {
        Serial.println("calibrateReaperTask | State 6: Doors closed, disabling REAPER STEPPER");
        digitalWrite(ENABLE_REAPER, HIGH);  // DISABLE REAPER STEPPER
        calibrateReaperTask.disable();      // Disable the task as it is complete
        state = 0;                          // Reset the state machine
        stepCount = 0;                      // Reset step count for the next activation
      }
      break;
  }
}

void reaperTaskCallback() {
  static int state = 0;
  static unsigned long previousMillis = 0;
  static unsigned long stepCount = 0;
  unsigned long currentMillis = millis();
  const unsigned long stepDelay = 3;
  const int stepSize = 5;               // Increased step size for better visibility in debug
  const unsigned long servoDelay = 15;  // Delay between each servo step in milliseconds

  static unsigned long startMillis = 0;
  const unsigned long disableSwitchDelay = 2000;

  switch (state) {
    case 0:
      Serial.println("reaperTask | State 0: Moving servos to open position");
      previousMillis = currentMillis;
      state = 1;
      break;

    case 1:
      // Move servos smoothly to open position
      if (doorRight.read() <= 0 && doorLeft.read() >= 179) {
        Serial.println("reaperTask | State 2: Doors open, move to step 2");
        state = 2;
      } else if (currentMillis - previousMillis >= servoDelay) {
        previousMillis = currentMillis;

        doorRight.write(0);
        doorLeft.write(180);
      }
      break;

    case 2:
      Serial.println("reaperTask | State 2: Enabling REAPER STEPPER");
      digitalWrite(ENABLE_REAPER, LOW);  // ENABLE REAPER STEPPER
      previousMillis = currentMillis;
      startMillis = currentMillis;
      stepCount = 0;
      state = 3;
      break;

    case 3:
      if (currentMillis - startMillis >= disableSwitchDelay && digitalRead(LIMIT_REAPER) == LOW) {
        Serial.println("reaperTask | State 3: Stepper movement complete, moving servos to closed position");
        state = 5;
      } else if (currentMillis - previousMillis >= stepDelay) {
        digitalWrite(STEP_REAPER, HIGH);
        previousMillis = currentMillis;
        state = 4;
      }
      break;

    case 4:
      if (currentMillis - previousMillis >= stepDelay) {
        digitalWrite(STEP_REAPER, LOW);
        stepCount++;
        previousMillis = currentMillis;
        state = 3;
      }
      break;

    case 5:
      Serial.println("reaperTask | State 5: Moving servos to closed position");
      previousMillis = currentMillis;
      state = 6;
      break;

    case 6:
      doorRight.write(closedServoPos);
      doorLeft.write(180 - closedServoPos);
      state = 7;

      break;

    case 7:
      Serial.println("reaperTask | State 7: Disabling REAPER STEPPER");
      digitalWrite(ENABLE_REAPER, HIGH);  // DISABLE REAPER STEPPER
      reaperTask.disable();               // Disable the task as it is complete
      state = 0;                          // Reset the state machine
      stepCount = 0;                      // Reset step count for the next activation
      break;
  }
}

void strikeBellTaskCallback() {
  // NOTE: This task only strikes the internal bell and not the skeleton bell.

  static int state = 0;
  static unsigned long strikeCount = 0;
  static unsigned long previousMillis = 0;
  unsigned long currentMillis = millis();
  const unsigned long delayBeforeStrike = 1000;  // Delay in milliseconds before strike (1000 ms / 1s)
  const unsigned long delayAfterStrike = 500;    // Delay in milliseconds after strike (500 ms / 0.5s)

  switch (state) {
    case 0:
      Serial.println("strikeBellTask | State 0: Striking bell");
      if (strikeCount < queuedStrikes) {
        previousMillis = currentMillis;
        state = 1;
      } else {
        state = 3;  // Move to the final state if strikes are complete
      }
      break;

    case 1:
      if (currentMillis - previousMillis >= delayBeforeStrike) {
        Serial.println("strikeBellTask | State 1: Activating solenoids for strike");
        digitalWrite(SOLENOID_BELL, HIGH);
        previousMillis = currentMillis;
        state = 2;
      }
      break;

    case 2:
      if (currentMillis - previousMillis >= delayAfterStrike) {
        Serial.println("strikeBellTask | State 2: Deactivating solenoids after strike");
        digitalWrite(SOLENOID_BELL, LOW);
        strikeCount++;
        Serial.print("Strike Count: ");
        Serial.println(strikeCount);
        if (strikeCount < queuedStrikes) {
          state = 0;  // Prepare for the next strike
        } else {
          state = 3;  // Move to the final state if all strikes are complete
        }
        previousMillis = currentMillis;
      }
      break;

    case 3:
      Serial.println("strikeBellTask | State 3: Strike task complete");
      strikeBellTask.disable();  // Disable the task as it is complete
      state = 0;                 // Reset the state machine
      strikeCount = 0;           // Reset strike count for the next activation
      break;
  }
}

void setup() {
  //Serial.begin(9600);
  SetPinModes();
  InitializeRTC();
  delay(500);
  InitializeDisplay();
  InitializeSteppers();
  InitializeServos();

  runner.init();
  runner.addTask(calibrateSkullTask);
  runner.addTask(skullTask);
  runner.addTask(calibrateReaperTask);
  runner.addTask(reaperTask);
  runner.addTask(strikeBellTask);

  calibrateSkullTask.restart();
  calibrateReaperTask.restart();
  //reaperTask.restart();
  //skullTask.restart();
}

void loop() {
  runner.execute();

  // Read the key module input
  inputValue = checkKeyInput();
  handleInput(inputValue);

  EventHandler();
  displayTime();
}