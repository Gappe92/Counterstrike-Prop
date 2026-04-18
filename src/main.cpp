// ===================== LIBRARIES =====================
// Core Arduino library
#include <Arduino.h>
// I2C communication for LCD
#include <Wire.h>
// LCD library for PCF8574 I2C backpack
#include <LiquidCrystal_PCF8574.h>
// Keypad library
#include <Keypad.h>
// DFPlayer library (not currently used in your code)
#include <DFRobotDFPlayerMini.h>
// Hardware serial (used for DFPlayer, optional here)
#include <HardwareSerial.h>

// Include sound arrays and their lengths
#include "soundArrays.h"

// ===================== PINS =====================

// LCD (I2C)
#define SCREEN_SDA 22
#define SCREEN_SCL 23

// Speaker pins
#define SPEAKER_PIN 26        // DAC output pin
#define SPEAKER_LOCK_PIN 14   // MOSFET/amp control
#define MAX_ACTIVE_SOUNDS 4   // max overlapping sounds

// Lever switch
#define SWITCH_PIN 12

// Keypad wiring pins
#define KEYBOARD_PIN1 15
#define KEYBOARD_PIN2 4 
#define KEYBOARD_PIN3 16
#define KEYBOARD_PIN4 17
#define KEYBOARD_PIN5 5
#define KEYBOARD_PIN6 18
#define KEYBOARD_PIN7 19
#define KEYBOARD_PIN8 21

// ===================== KEYPAD =====================

// 4x4 keypad layout
char keys[4][4] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// Row and column pins for Keypad library
byte rowPins[4] = { KEYBOARD_PIN5, KEYBOARD_PIN6, KEYBOARD_PIN7, KEYBOARD_PIN8 };
byte colPins[4] = { KEYBOARD_PIN1, KEYBOARD_PIN2, KEYBOARD_PIN3, KEYBOARD_PIN4 };

// Initialize keypad object
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, 4, 4);

// ===================== ENUMS =====================

// Sound type enumeration
enum Sound
{
  keypress,
  beep,
  planting,
  planted,
  defusing,
  defused,
  detonated
};

// Text alignment enum for LCD printing
enum TextAlign
{
  ALIGN_LEFT,
  ALIGN_CENTER,
  ALIGN_RIGHT
};

// ===================== STRUCTS =====================

// Represents a single active sound instance for mixing
struct SoundInstance
{
    const uint8_t* data;  // pointer to audio array
    int length;           // total length of sound
    int index;            // current playhead index
    bool active;          // whether sound is currently playing
};

// ===================== LCD ===========================

// Define LCD object for I2C at address 0x27
LiquidCrystal_PCF8574 lcd(0x27);

// ===================== GAME VARIABLES =====================

// Default codes for the game
String startCode = "3983";
String detonationCode = "7355608";

// Timing values (seconds)
uint32_t detonationTime = 30;
uint32_t defuseTime     = 5;
uint32_t plantTime      = 120;

// =================== TIMER & SOUND VARIABLES ====================

// Hardware timer for playing sound at precise intervals
hw_timer_t * timer = nullptr;

// Variables for the currently playing sound (deprecated single sound)
volatile int soundIndex = 0;
const uint8_t* currentSound = nullptr;
volatile int currentLength = 0;

// Active sound mixing system
volatile uint8_t activeSounds = 0;                     // counts how many sounds are playing
volatile SoundInstance activeList[MAX_ACTIVE_SOUNDS];  // array of active sounds
volatile uint8_t activeCount = 0;                      // number of active sounds

// =================== SOUND PLAYBACK INTERRUPT ====================

// Called by hardware timer at fixed intervals to output sound
void IRAM_ATTR onTimer()
{
    if (activeCount == 0)
    {
        // No sound playing: output silence and turn off amp
        dacWrite(SPEAKER_PIN, 128);  // midpoint value = silence
        digitalWrite(SPEAKER_LOCK_PIN, HIGH);  // amp OFF
        return;
    }

    int mixed = 0;        // accumulator for mixing multiple sounds
    uint8_t playingNow = 0;

    // Loop through all active sound slots
    for (int i = 0; i < MAX_ACTIVE_SOUNDS; i++)
    {
        if (activeList[i].active)
        {
            if (activeList[i].index < activeList[i].length)
            {
                // Add current sample to mixed output
                mixed += activeList[i].data[activeList[i].index++];
                playingNow++;
            }
            else
            {
                // Sound finished
                activeList[i].active = false;
                if (activeCount > 0)
                    activeCount--;
            }
        }
    }

    if (playingNow > 0)
    {
        // Average all samples to prevent overflow
        mixed /= playingNow;
        dacWrite(SPEAKER_PIN, mixed);
    }
}

// ===================== GENERAL FUNCTIONS =====================

// Plays a given sound (non-blocking)
void PlaySound(Sound sound)
{
    const uint8_t* data = nullptr;
    int length = 0;

    // Map enum to audio array and length
    switch(sound)
    {
        case keypress:
            data = clickSound;
            length = clickLength;
            break;

        case planted:
            data = plantedSound;
            length = plantedLen;
            break;

        case beep:
            data = beepSound;
            length = beepLen;
            break;

        case detonated:
            data = detonatedSound;
            length = detonatedLen;
            break;
    }

    if (!data) return;

    // Find a free slot in active sound array
    for (int i = 0; i < MAX_ACTIVE_SOUNDS; i++)
    {
        if (!activeList[i].active)
        {
            activeList[i].data = data;
            activeList[i].length = length;
            activeList[i].index = 0;
            activeList[i].active = true;

            activeCount++;                    // increment active count
            digitalWrite(SPEAKER_LOCK_PIN, LOW);  // amp ON
            break;
        }
    }
}

// Write a string to the LCD at given line and alignment
void WriteLine(const String& value, TextAlign align, bool clearLine, int line)
{
  int len = value.length();
  if (len > 16) len = 16;  // truncate if longer than 16 chars

  int startPos = 0;
  if (align == ALIGN_CENTER) startPos = (16 - len) / 2;
  else if (align == ALIGN_RIGHT) startPos = 16 - len;

  // Clear line if requested
  if (clearLine)
  {
    lcd.setCursor(0, line);
    lcd.print("                "); // 16 spaces
  }

  lcd.setCursor(startPos, line);
  lcd.print(value.substring(0, len));
}

// Reads string input from keypad until A/D pressed
String ReadStringFromKeypad(const String& title, bool time, String defaultValue)
{
  String buffer = defaultValue;

  // Show title and default value on LCD
  WriteLine(title, ALIGN_CENTER, true, 0);
  WriteLine(time ? defaultValue+"s" : defaultValue, ALIGN_CENTER, true, 1);

  while (true)
  {
    char key = keypad.getKey();
    if (key)
    {
      PlaySound(keypress);  // play keypress sound

      if (key == 'D' || key == 'A')
      {
        // Accept input if buffer not empty for time
        if ((time && buffer.length() > 0) || !time)
            return buffer;
      }

      // Backspace
      if (key == 'B' && buffer.length() > 0)
          buffer.remove(buffer.length() - 1);

      // Clear
      if (key == 'C') buffer = "";

      // Digits
      if (key >= '0' && key <= '9')
      {
        if (time && buffer.length() < 9) buffer += key;      // max 9 digits for time
        else if (!time && buffer.length() < 16) buffer += key; // max 16 chars for code
      }

      // Update display
      if(buffer != "")
        WriteLine(buffer + (time? "s":""), ALIGN_CENTER, true, 1);
      else
        WriteLine(time?"0s":"", ALIGN_CENTER, true, 1);
    }
  }
}

// Draw a horizontal progress bar on the LCD
void DrawProgressBar(uint8_t percent, int line)
{
  percent = constrain(percent, 0, 100);
  uint8_t filled = map(percent, 0, 100, 0, 16);

  lcd.setCursor(0, line);
  for (uint8_t i = 0; i < 16; i++)
  {
    if (i < filled) lcd.print((char)255); // filled block
    else lcd.print(' ');                  // empty space
  }
}

// ===================== SETUP =====================

void setup()
{
  setCpuFrequencyMhz(240);  // set ESP32 CPU to 240 MHz

  Serial.begin(9600);        // debug output

  Wire.begin(SCREEN_SDA, SCREEN_SCL);
  lcd.begin(16, 2);
  lcd.setBacklight(true);

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(SPEAKER_LOCK_PIN, OUTPUT);
  digitalWrite(SPEAKER_LOCK_PIN,HIGH); // amp OFF initially

  // Initial splash screen
  WriteLine("TACTICAL", ALIGN_CENTER, true, 0);
  WriteLine("ORDNANCE", ALIGN_CENTER, true, 1);
  delay(2000);

  // Keypad settings for responsiveness
  keypad.setHoldTime(50);    // ms until HOLD detected
  keypad.setDebounceTime(30); // debounce time for keypresses

  // Configure hardware timer for sound playback
  timer = timerBegin(0, 80, true);        // 80 prescaler = 1us tick
  timerAttachInterrupt(timer, &onTimer, true);
  timerAlarmWrite(timer, 1000000 / 16000, true); // 16 kHz sample rate
  timerAlarmEnable(timer);

  // Read initial user configuration via keypad
  startCode = ReadStringFromKeypad("Start Code",false, startCode);
  detonationCode = ReadStringFromKeypad("Detonation Code",false, detonationCode);
  plantTime = ReadStringFromKeypad("Plant Time",true, String(plantTime)).toInt();
  detonationTime = ReadStringFromKeypad("Detonation Time",true, String(detonationTime)).toInt();
  defuseTime = ReadStringFromKeypad("Defuse Time",true, String(defuseTime)).toInt();

  // Debug print of initial values
  Serial.print("Start Code "); Serial.println(startCode);
  Serial.print("Detonation Code "); Serial.println(detonationCode);
  Serial.print("Plant Time "); Serial.println(plantTime);
  Serial.print("Detonation Time "); Serial.println(detonationTime);
  Serial.print("Defuse Time "); Serial.println(defuseTime);
}

// ===================== GAME STATES =====================

// Bomb state enumeration
enum BombState {
  WAITING_START,
  AWAIT_PLANT,
  ENTER_CODE,
  AWAIT_LOWER_LEVER,
  PLANTED,
  DEFUSING,
  DEFUSED,
  DETONATED,
  NOTIMELEFT
};

// Game state variables
bool awaitWritten = false;
BombState state = WAITING_START;
BombState oldState = DEFUSED;
String buffer="";
bool invalid = false;
int timerStartedAt=0;
int defuseStart=0;
static bool defuseActive = false;
uint32_t defuseTimeLeftMs = 0;


void loop() {
  unsigned long elapsed;
  long timeLeft;
  uint32_t displayTime = 0;
  switch (state)
  {
    case WAITING_START:
    
      if(startCode!="")
      {
        if(oldState!=WAITING_START)
        {
          defuseActive = false;
          WriteLine("Enter Start Code",ALIGN_CENTER,true,0);
          WriteLine("",ALIGN_CENTER,true,1);
          buffer="";
        }
        while (true)
        {
          char key = keypad.getKey();
          if (key)
          {
            PlaySound(keypress);
            if(invalid && (key == 'A'|| key == 'D'))
            {
              WriteLine("Enter Start Code",ALIGN_CENTER,true,0);
              invalid =false;
            }
            else if(!invalid)
            {
              if (key == 'D' || key == 'A')
              {
                if(buffer==startCode)
                {
                  if(digitalRead(SWITCH_PIN) == LOW)
                  {
                    WriteLine("Turn the",ALIGN_CENTER,true,0);
                    WriteLine("Switch OFF",ALIGN_CENTER,true,1);
                    while (true)
                    {
                      key = keypad.getKey();
                      if(key && (key == 'A'|| key == 'D'))
                      {
                        PlaySound(keypress);
                        WriteLine("Enter Start Code",ALIGN_CENTER,true,0);
                        WriteLine("",ALIGN_CENTER,true,1);
                        break;
                      }
                    }
                  }
                  else
                  {
                    buffer="0";
                    state = AWAIT_PLANT;
                    timerStartedAt=millis();
                    break;
                  }
                }
                else
                {
                  buffer = "";
                  //print error message
                  WriteLine("Invalid Code",ALIGN_CENTER,true,0);
                  invalid = true;
                }
              }

              // One Char back
              if (key == 'B') {
                if (buffer.length() > 0) {
                  buffer.remove(buffer.length() - 1);
                }
              }

              // Clear
              if (key == 'C')
              {
                buffer = "";
              }

              // Digits
              if (key >= '0' && key <= '9')
              {
                if (buffer.length() < 16)  // max 16 chars for code
                  buffer += key;
              }

              if(buffer != "") 
                WriteLine(buffer, ALIGN_CENTER, true, 1);
              else  
                WriteLine("", ALIGN_CENTER, true, 1);
            }
          }
        }
      }
      else
      {
        if(oldState!=WAITING_START)
        {
          WriteLine("Press 'A'",ALIGN_CENTER,true,0);
          WriteLine("To Start",ALIGN_CENTER,true,1);
        }
        while (true)
        {
          char key = keypad.getKey();
          if(key && key == 'A')
          {
            if(digitalRead(SWITCH_PIN) == LOW)
            {
              WriteLine("Turn the",ALIGN_CENTER,true,0);
              WriteLine("Switch OFF",ALIGN_CENTER,true,1);
              while (true)
              {
                key = keypad.getKey();
                if(key && key == 'A')
                {
                  WriteLine("Press 'A'",ALIGN_CENTER,true,0);
                  WriteLine("To Start!",ALIGN_CENTER,true,1);
                  break;
                }
              }
            }
            else
            {
              state = AWAIT_PLANT;
              timerStartedAt=millis();
              PlaySound(keypress);
              break;
            }
          }
        }
      }
      oldState = WAITING_START;
      break;

    case AWAIT_PLANT:
      elapsed = (millis() - timerStartedAt) / 1000;
      timeLeft = (plantTime) - elapsed;
      if(timeLeft == -1)
      {
        state = NOTIMELEFT;
      }
      displayTime = timeLeft;

      static uint32_t lastSecond = displayTime;
      if (lastSecond != displayTime) {
          WriteLine("Time: " + String(displayTime), ALIGN_CENTER, true, 1);
          lastSecond = displayTime;
      }

      if(oldState!=AWAIT_PLANT)
      {
        WriteLine("AWAITING PLANT",ALIGN_CENTER,true,0);
        WriteLine("Time: " + String(displayTime), ALIGN_CENTER, true, 1);
        oldState = AWAIT_PLANT;
      }
      if(digitalRead(SWITCH_PIN) == LOW)
      {
        state = ENTER_CODE;
        invalid=false;
      }
      break;

    case ENTER_CODE:
      PlaySound(beep);
      if(oldState!=ENTER_CODE)
      {
        WriteLine("Enter Code:",ALIGN_CENTER,true,0);
        WriteLine("",ALIGN_CENTER,true,1);
        oldState = ENTER_CODE;
      }
      buffer ="";
      while (true)
      {
        if(digitalRead(SWITCH_PIN) == HIGH)
        {
          state = AWAIT_PLANT;
          break;
        }
        elapsed = (millis() - timerStartedAt) / 1000;
        timeLeft = (plantTime) - elapsed;
        if(timeLeft == -1)
        {
          state = NOTIMELEFT;
          break;
        }
        char key = keypad.getKey();
        if (key)
        {
          PlaySound(keypress);
          if(invalid && (key =='A'||key == 'D'))
          {
            WriteLine("Enter Code",ALIGN_CENTER,true,0);
            invalid =false;
          }
          else if(!invalid)
          {
            if (key == 'D' || key == 'A')
            {
              if(buffer==detonationCode)
              {
                state = AWAIT_LOWER_LEVER;
                break;
              }
                
              else
              {
                buffer = "";
                //print error message
                WriteLine("Invalid Code",ALIGN_CENTER,true,0);
                WriteLine("",ALIGN_CENTER,true,1);
                invalid = true;
              }
            }

            // One Char back
            if (key == 'B') {
              if (buffer.length() > 0) {
                buffer.remove(buffer.length() - 1);
              }
            }

            // Clear
            if (key == 'C')
            {
              buffer = "";
            }

            // Digits
            if (key >= '0' && key <= '9')
            {
              if (buffer.length() < 16)  // max 16 chars for code
                buffer += key;
            }

            if(buffer != "") 
              WriteLine(buffer, ALIGN_CENTER, true, 1);
            else  
              WriteLine("", ALIGN_CENTER, true, 1);
          }
        }
      }
      if(digitalRead(SWITCH_PIN) == HIGH)
      {
        state = AWAIT_PLANT;
      }
      break;

    

    case AWAIT_LOWER_LEVER:
      PlaySound(keypress);
      elapsed = (millis() - timerStartedAt) / 1000;
      timeLeft = (plantTime) - elapsed;
      if(timeLeft == -1)
      {
        state = NOTIMELEFT;
        break;
      }
      displayTime = timeLeft;
      WriteLine("TURN SWITCH OFF!",ALIGN_CENTER,true,0);
      WriteLine("Time: " + String(displayTime), ALIGN_CENTER, true, 1);
      while(true)
      {
        elapsed = (millis() - timerStartedAt) / 1000;
        timeLeft = (plantTime) - elapsed;
        if(timeLeft == -1)
        {
          state = NOTIMELEFT;
          break;
        }
        displayTime = timeLeft;

        static uint32_t lastSecond = displayTime;
        if (lastSecond != displayTime) {
            WriteLine("Time: " + String(displayTime), ALIGN_CENTER, true, 1);
            lastSecond = displayTime;
        }

        if(digitalRead(SWITCH_PIN) == HIGH)
        {
          PlaySound(keypress);
          state=PLANTED;
          break;
        }

      }
      break;

    case PLANTED:
    {
        static uint32_t lastSecond = UINT32_MAX;
        static uint32_t lastBeep = 0;

        // Start timer ONLY once when entering state
        if (oldState != PLANTED)
        {
            timerStartedAt = millis();
            lastSecond = UINT32_MAX;
            lastBeep = 0;
            PlaySound(beep);

            WriteLine("BOMB PLANTED", ALIGN_CENTER, true, 0);
            WriteLine("Time: " + String(detonationTime), ALIGN_CENTER, true, 1);

            oldState = PLANTED;
        }

        // --- TIMER ---
        elapsed = (millis() - timerStartedAt) / 1000;

        // If timer is up, bomb detonates
        if (elapsed >= detonationTime)
        {
            state = DETONATED;
            break;
        }

        // Calculate remaining time
        timeLeft = detonationTime - ((millis() - timerStartedAt) / 1000);

        // Update LCD every second if not defusing
        if ((timeLeft != lastSecond) && !defuseActive)
        {
            WriteLine("Time:", ALIGN_LEFT, true, 1);
            WriteLine(String(timeLeft), ALIGN_RIGHT, false, 1);
            lastSecond = timeLeft;
        }

        // --- BEEP INTERVAL ---
        uint32_t beepInterval = 1000; // default 1 beep/sec
        if (timeLeft <= 15 && timeLeft >= 8) beepInterval = 500;   // twice as fast
        else if (timeLeft >= 4 && timeLeft < 8) beepInterval = 125;
        else if (timeLeft < 4) beepInterval = 67;                  // four times as fast

        if (millis() - lastBeep >= beepInterval)
        {
            PlaySound(beep);
            lastBeep = millis();
        }

        // --- DEFUSE HOLD ---
        keypad.getKeys();

        for (int i = 0; i < LIST_MAX; i++)
        {
            if (keypad.key[i].kchar == 'D')
            {
                if (keypad.key[i].kstate == HOLD)
                {
                    if (!defuseActive)
                    {
                        defuseActive = true;
                        defuseStart = millis();  // START HERE
                        WriteLine("DEFUSING", ALIGN_CENTER, true, 0);
                    }

                    uint32_t held = millis() - defuseStart;
                    uint8_t percent = map(
                        held,
                        0,
                        defuseTime * 1000UL,
                        0,
                        100
                    );

                    DrawProgressBar(percent, 1);

                    // --- DEFUSE COMPLETE ---
                    if (held >= defuseTime * 1000UL)
                    {
                        defuseActive = false;

                        // Capture exact remaining time in milliseconds
                        defuseTimeLeftMs = detonationTime * 1000UL - (millis() - timerStartedAt);

                        state = DEFUSED;
                        break;  // exit loop immediately
                    }
                }

                // If released before fully defusing
                if (keypad.key[i].kstate == RELEASED)
                {
                    defuseActive = false;
                    WriteLine("BOMB PLANTED", ALIGN_CENTER, true, 0);
                    DrawProgressBar(0, 1);
                }
            }
        }
        break;
    }


      case DEFUSED:
        if(oldState != state)
        {
            WriteLine("BOMB DEFUSED", ALIGN_CENTER, true, 0);

            // Show how much time was left in milliseconds
            uint32_t sec = defuseTimeLeftMs / 1000;
            uint32_t ms  = defuseTimeLeftMs % 1000;
            WriteLine("Time left: " + String(sec) + "." + String(ms) + "s", ALIGN_CENTER, true, 1);

            oldState = DEFUSED;
        }
        // Wait for user to press 'A' to reset
        while (true)
        {
            char key = keypad.getKey();
            if(key && key == 'A')
            {
                state = WAITING_START;
                PlaySound(keypress);
                break;
            }
        }
        break;

    case DETONATED:
        if(oldState != state)
        {
            PlaySound(detonated);
            WriteLine("BOMB DETONATED", ALIGN_CENTER, true, 0);
            WriteLine("", ALIGN_CENTER, true, 1);
            oldState = DETONATED;
        }
        while (true)
        {
            char key = keypad.getKey();
            if(key && key == 'A')
            {
                state = WAITING_START;
                PlaySound(keypress);
                break;
            }
        }
        break;

    case NOTIMELEFT:
        WriteLine("Game Over!", ALIGN_CENTER, true, 0);
        WriteLine("No time left", ALIGN_CENTER, true, 1);
        while (true)
        {
            char key = keypad.getKey();
            if(key && key == 'A')
            {
                state = WAITING_START;
                PlaySound(keypress);
                break;
            }
        }
        break;
    default:
      break;
  }
}