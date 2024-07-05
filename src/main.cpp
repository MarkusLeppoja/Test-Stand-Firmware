#include <Arduino.h>
#include <SdFat.h>
#include <U8g2lib.h>
#include <HX711_ADC.h>

/* Pre-Defined */

// GPIO
#define GPIO_THERMISTOR_1                     24
#define GPIO_THERMISTOR_2                     25
#define GPIO_LOAD_CELL_SCK                    13
#define GPIO_LOAD_CELL_DT                     6
#define GPIO_RELAY_TOGGLE                     14
#define GPIO_DISPLAY_SCL                      19
#define GPIO_DISPLAY_SDA                      18
#define GPIO_LED_TEST_ACTIVE                  32
#define GPIO_BUTTON_ACTIVATE_TEST             33

/* User Configurable */

// Test config
#define TEST_DATA_SAMPLE_RATE           50
#define TEST_COUNTDOWN_SECONDS          30
#define TEST_DURATION_SECONDS           15

// Sensor calibration
#define LOAD_CELL_CALIBRATION_VALUE     1
#define THERMISTOR_1_RESISTANCE         19750
#define THERMISTOR_2_RESISTANCE         18550

enum E_OPERATION_STATE : uint8_t {
  STARTUP = 0,
  ERROR =  1,
  READY_FOR_COUNTDOWN = 2,
  COUNTDOWN = 3,
  TEST_ACTIVE = 4,
  POST_TEST = 5
};
E_OPERATION_STATE OPERATION_STATE;

String S_OPERATION_STATE []{
  "STARTUP",
  "ERROR",
  "READY_FOR_COUNTDOWN",
  "COUNTDOWN" ,
  "TEST_ACTIVE",
  "POST_TEST"
};

/* Function Definitions */

// Initializers
void InitSerial(void);
void InitRecorder(void);
void InitLoadCell(void);
void InitDisplay(void);
void InitGPIO(void);

// Operational functions
void GetThermistorData(void);
void GetLoadCellData(void);
void LogTestData(void);

// Specific commands
void LoadCellTare(void);
float ReadThermistor(const int Pin, float Resistance, float CalibrationOffset);
void InterruptTestStartCommand(void);
void TestEndCommand(void);
void DisplayRenderData(void);
boolean CreateLogFile(void);
void CloseLogFile(void);
void DetectCountdownEnd(void);
void BeginTest(void);
void DetectTestEnd(void);
void EndTest(void);
void ToggleRelay(boolean Status);
void CreateTelemetryString(void);

/* Other Definitions */

// Countdown 
uint64_t CountdownActivatedTime = 10^10;
uint64_t TestActivatedTime = 10^10;
float Countdown = TEST_COUNTDOWN_SECONDS;
float TestDuration = TEST_DURATION_SECONDS;

// Sensor data
float ThermistorData[2];
float LoadCellForceData;

// Recorder 
SdFs Sd;
File32 File;

// Load cell
HX711_ADC LoadCell(GPIO_LOAD_CELL_DT, GPIO_LOAD_CELL_SCK);

// Display
U8G2_SSD1306_128X64_NONAME_1_SW_I2C Display(U8G2_R0, GPIO_DISPLAY_SCL, GPIO_DISPLAY_SDA, U8X8_PIN_NONE);

// Debug
String ErrorLog = "";

// Loops
u_int64_t MainLoopPrev;


void InitSerial(void)
{
  Serial.begin(115200);
}

void InitRecorder(void)
{
  if (!Sd.begin(SdioConfig(FIFO_SDIO)))
  {
    OPERATION_STATE = E_OPERATION_STATE::ERROR;
    ErrorLog.append("SD-CARD NOT FOUND | ");
    return;
  }
}

void InitLoadCell(void)
{
  LoadCell.begin();
  LoadCell.start(400, true); // Settling time = SAMPLES + IGN_HIGH_SAMPLE + IGN_LOW_SAMPLE / SPS

  if (LoadCell.getTareTimeoutFlag()) 
  {
    OPERATION_STATE = E_OPERATION_STATE::ERROR;
    ErrorLog.append("LOAD CELL TARE UNSUCESSFUL | ");
  }

  LoadCell.setCalFactor(LOAD_CELL_CALIBRATION_VALUE); 
}

void InitDisplay(void)
{
  Display.begin();
}

void InitGPIO(void)
{
  pinMode(GPIO_THERMISTOR_1, INPUT);
  pinMode(GPIO_THERMISTOR_2, INPUT);
  pinMode(GPIO_RELAY_TOGGLE, OUTPUT);
  pinMode(GPIO_LED_TEST_ACTIVE, OUTPUT);
  pinMode(GPIO_BUTTON_ACTIVATE_TEST, INPUT);
  attachInterrupt(GPIO_BUTTON_ACTIVATE_TEST, InterruptTestStartCommand, HIGH);
}

void InterruptTestStartCommand(void)
{
  if (OPERATION_STATE != E_OPERATION_STATE::READY_FOR_COUNTDOWN) return;
  
  // Configure for test
  uint8_t ErrorCount = 0;

  digitalWrite(GPIO_LED_TEST_ACTIVE, HIGH);
  CountdownActivatedTime = millis();
  if (!CreateLogFile()) ErrorCount++;
  CreateTelemetryString();

  if (ErrorCount != 0)
  {
    OPERATION_STATE = E_OPERATION_STATE::ERROR;
    ErrorLog.append("STARTUP NOT SUCESSFUL | ");
    return;
  }
  OPERATION_STATE = E_OPERATION_STATE::COUNTDOWN;
}

void CreateTelemetryString(void)
{
  String TelemetryString;
  TelemetryString.append("Time (s), Force (N), Temperature #1 (*C), Temperature #2 (*C)\n");
  File.printf(TelemetryString.c_str());
  File.sync();
}

void TestEndCommand(void)
{
  if (OPERATION_STATE != E_OPERATION_STATE::TEST_ACTIVE) return;
  OPERATION_STATE = E_OPERATION_STATE::POST_TEST;

  digitalWrite(GPIO_LED_TEST_ACTIVE, LOW);
  CloseLogFile();
}

void DisplayRenderData(void)
{
  Display.firstPage();

  /* Layout
  * STATE
  * ERROR CODE
  * TELEMETRY
  */ 

  do {
    Display.drawHLine(5, 0, 120);
    Display.drawHLine(5, 10, 120);

    Display.setFont(u8g2_font_3x5im_mr);
    Display.drawStr(5, 8, String(S_OPERATION_STATE[OPERATION_STATE]).c_str());
    Display.drawStr(5, 19, String(ErrorLog).c_str());

    Display.drawStr(5, 42, "Load Cell     =");
    Display.drawStr(5, 52, "Thermistor #1 =");
    Display.drawStr(5, 62, "Thermistor #2 =");

    Display.drawStr(70, 42, String(LoadCellForceData).c_str());
    Display.drawStr(70, 52, String(ThermistorData[0]).c_str());
    Display.drawStr(70, 62, String(ThermistorData[1]).c_str());

    Display.setFont(u8g2_font_10x20_me);
    Display.drawStr(96, 56, String(int(TEST_COUNTDOWN_SECONDS - Countdown)).c_str());
  } while (Display.nextPage());

}

void LoadCellTare(void)
{
  LoadCell.tareNoDelay();
}

void CloseLogFile(void)
{
  File.close();
}

void DetectCountdownEnd(void)
{
  if (OPERATION_STATE != E_OPERATION_STATE::COUNTDOWN) return;
  
  Countdown = float(millis() - CountdownActivatedTime) / 1000.f;

  if (Countdown < TEST_COUNTDOWN_SECONDS) return;
  BeginTest();
}

void BeginTest(void)
{
  TestActivatedTime = millis();
  OPERATION_STATE = E_OPERATION_STATE::TEST_ACTIVE;

  ToggleRelay(true);
}

void DetectTestEnd(void)
{
  if (OPERATION_STATE != E_OPERATION_STATE::TEST_ACTIVE) return;

  TestDuration = float(millis() - TestActivatedTime) / 1000.f;

  // For displaying test duration
  Countdown = TestDuration + 15;

  if (TestDuration < TEST_DURATION_SECONDS) return;
  EndTest();
}

void EndTest(void)
{
  OPERATION_STATE = E_OPERATION_STATE::POST_TEST;
  digitalWrite(GPIO_LED_TEST_ACTIVE, LOW);
  CloseLogFile();
  ToggleRelay(false);
}

void LogTestData(void)
{
  String TelemetryString;
  TelemetryString.append(millis() / 1000.f);
  TelemetryString.append(", ");
  TelemetryString.append(double(LoadCellForceData));
  TelemetryString.append(", ");
  TelemetryString.append(double(ThermistorData[0]));
  TelemetryString.append(", ");
  TelemetryString.append(double(ThermistorData[1]));
  TelemetryString.append("\n");

  File.printf(TelemetryString.c_str());
  File.sync();
}

boolean CreateLogFile(void)
{
  String filename = "Motor Test Data #" + String(random(100)) + ".csv";
  while (Sd.exists(filename))
  {
    filename = "Motor Test Data #" + String(random(100)) + ".csv";
  }
  
  return File.open(filename.c_str(), FILE_WRITE);
}

void ToggleRelay(boolean Status)
{
  if (Status == false)
  {
    digitalWrite(GPIO_RELAY_TOGGLE, HIGH);
    return;
  }
  digitalWrite(GPIO_RELAY_TOGGLE, LOW);
}

void GetThermistorData(void)
{
  ThermistorData[0] = ReadThermistor(GPIO_THERMISTOR_1, THERMISTOR_1_RESISTANCE, 40);
  ThermistorData[1] = ReadThermistor(GPIO_THERMISTOR_2, THERMISTOR_2_RESISTANCE, 40);
}

float Vo, R1, logR2, T;
const float c1 = 1.009249522e-03, c2 = 2.378405444e-04, c3 = 2.019202697e-07;
float ReadThermistor(const int Pin, float Resistance, float CalibrationOffset)
{
  Vo = (3.3f / 1024.0f) * analogRead(Pin);
  R1 = (float) Resistance * (3.3f - Vo) / Vo;
  logR2 = log(R1);
  T = (1.0 / (c1 + c2*logR2 + c3*logR2*logR2*logR2));
  T = T - 273.15;
  return T + CalibrationOffset;
}

void GetLoadCellData(void)
{
  if (LoadCell.update())
  {
    LoadCellForceData = LoadCell.getData() / 100000.f;
  }
}

void setup(void) 
{
  // Initializers
  InitGPIO();
  InitRecorder();
  InitLoadCell();
  InitDisplay();
  
  // Check if startup is sucessful
  if (OPERATION_STATE == E_OPERATION_STATE::STARTUP && OPERATION_STATE != E_OPERATION_STATE::ERROR)
  {
    OPERATION_STATE = E_OPERATION_STATE::READY_FOR_COUNTDOWN;
  }
}

void loop(void)
{
  if (millis() - MainLoopPrev >= (1000.f / TEST_DATA_SAMPLE_RATE))
  {
    DisplayRenderData();

    switch (OPERATION_STATE)
    {

    case READY_FOR_COUNTDOWN:
      GetThermistorData();
      GetLoadCellData();
      break;

    case COUNTDOWN:
      GetThermistorData();
      GetLoadCellData();
      LogTestData();
      DetectCountdownEnd();
      break;

    case TEST_ACTIVE:
      GetThermistorData();
      GetLoadCellData();
      LogTestData();
      DetectTestEnd();
      break;

    case POST_TEST:
      GetThermistorData();
      GetLoadCellData();
      break;
    
    default:
      break;
    }
    MainLoopPrev = millis();
  }
}