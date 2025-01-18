/**
 * Audio Guestbook, Copyright (c) 2022 Playful Technology
 * 
 * Tested using a Teensy 4.0 with Teensy Audio Shield, although should work 
 * with minor modifications on other similar hardware
 * 
 * When handset is lifted, a pre-recorded greeting message is played to ask users to select different options using the dial.
 * Depending on the dialed selection, a different greeting message is played, followed by a tone.
 * Then, recording starts, and continues until the handset is replaced, or a number is dialled again.
 * The message just recorded will be played back to the user.
 * There are also hidden 'secret' functions available at the initial greeting stage, depending on the number dialled.
 * 
 * Files are saved on SD card as WAV recordings
 * 
 * GNU GPL v3.0 license
 * 
 */

#include <Bounce.h>
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <TimeLib.h>
#include <MTP_Teensy.h>
#include "play_sd_wav.h" // local copy with fixes

// DEFINES
// Define pins used by Teensy Audio Shield
#define SDCARD_CS_PIN    10
#define SDCARD_MOSI_PIN  7
#define SDCARD_SCK_PIN   14
// And those used for inputs
#define HOOK_PIN 0 // handset
#define PLAYBACK_BUTTON_PIN 1 // playback button
#define ROTARY_ENGAGE_PIN 2 // rotary dial - detects when dial starts to turn
#define ROTARY_ENCODE_PIN 3 // rotary dial - detects the number dialled

#define noINSTRUMENT_SD_WRITE

// GLOBALS
// Audio initialisation code can be generated using the GUI interface at https://www.pjrc.com/teensy/gui/
// Inputs
AudioSynthWaveform          waveform1; // To create the "beep" sfx
AudioInputI2S               i2s2; // I2S input from microphone on audio shield
AudioPlaySdWavX              playWav1; // Play 44.1kHz 16-bit PCM greeting WAV file
AudioRecordQueue            queue1; // Creating an audio buffer in memory before saving to SD
AudioMixer4                 mixer; // Allows merging several inputs to same output
AudioOutputI2S              i2s1; // I2S interface to Speaker/Line Out on Audio shield
AudioConnection patchCord1(waveform1, 0, mixer, 0); // wave to mixer 
AudioConnection patchCord3(playWav1, 0, mixer, 1); // wav file playback mixer
AudioConnection patchCord4(mixer, 0, i2s1, 0); // mixer output to speaker (L)
AudioConnection patchCord6(mixer, 0, i2s1, 1); // mixer output to speaker (R)
AudioConnection patchCord5(i2s2, 0, queue1, 0); // mic input to queue (L)
AudioControlSGTL5000      sgtl5000_1;

// Filename to save audio recording on SD card
char filename[15];
// The file object itself
File frec;

// Use long 40ms debounce time on both switches, and short 15ms debounce time for encoding dial mechanism
Bounce buttonRecord = Bounce(HOOK_PIN, 40);
Bounce buttonPlay = Bounce(PLAYBACK_BUTTON_PIN, 40);
Bounce rotaryEngage = Bounce(ROTARY_ENGAGE_PIN, 40);
Bounce rotaryEncode = Bounce(ROTARY_ENCODE_PIN, 15);

// Keep track of current state of the device
enum Mode {Initialising, Ready, Prompting, Recording, PostRecording, Playing};
Mode mode = Mode::Initialising;

// Keep track of the selected language
enum Lang {EN, ES, FR, JA};
Lang lang = Lang::EN;

// Keep track of the system inputs
enum Input {NoInput, HungUp, PlayButtonDown, PlayButtonUp, Dial1, Dial2, Dial3, Dial4, Dial5, Dial6, Dial7, Dial8, Dial9, Dial0};

float beep_volume = 0.03f; // not too loud :-)

uint32_t MTPcheckInterval; // default value of device check interval [ms]

// variables for writing to WAV file
unsigned long ChunkSize = 0L;
unsigned long Subchunk1Size = 16;
unsigned int AudioFormat = 1;
unsigned int numChannels = 1;
unsigned long sampleRate = 44100;
unsigned int bitsPerSample = 16;
unsigned long byteRate = sampleRate*numChannels*(bitsPerSample/8);// samplerate x channels x (bitspersample / 8)
unsigned int blockAlign = numChannels*bitsPerSample/8;
unsigned long Subchunk2Size = 0L;
unsigned long recByteSaved = 0L;
unsigned long NumSamples = 0L;
byte byte1, byte2, byte3, byte4;


void setup() {

  Serial.begin(9600);
  while (!Serial && millis() < 6000) {
    // wait for serial port to connect. Must be larger than 6000 for enough setup time
  }
  Serial.println("Serial set up correctly");
  Serial.printf("Audio block set to %d samples\n",AUDIO_BLOCK_SAMPLES);
  print_mode();
  // Configure the input pins
  pinMode(HOOK_PIN, INPUT_PULLUP);
  pinMode(PLAYBACK_BUTTON_PIN, INPUT_PULLUP);
  pinMode(ROTARY_ENGAGE_PIN, INPUT_PULLUP);
  pinMode(ROTARY_ENCODE_PIN, INPUT_PULLUP);

  // Audio connections require memory, and the record queue uses this memory to buffer incoming audio.
  AudioMemory(60);

  // Enable the audio shield, select input, and enable output
  sgtl5000_1.enable();
  // Define which input on the audio shield to use (AUDIO_INPUT_LINEIN / AUDIO_INPUT_MIC)
  sgtl5000_1.inputSelect(AUDIO_INPUT_MIC);
  //sgtl5000_1.adcHighPassFilterDisable()
  sgtl5000_1.volume(0.75);

  mixer.gain(0, 1.0f);
  mixer.gain(1, 1.0f);

  // Play a beep to indicate system is online
  waveform1.begin(beep_volume, 440, WAVEFORM_SINE);
  wait(1000);
  waveform1.amplitude(0);
  delay(1000);

  // Initialize the SD card
  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);
  if (!(SD.begin(SDCARD_CS_PIN))) 
  {
    // stop here if no SD card, but print a message
    while (1) {
      Serial.println("Unable to access the SD card");
      delay(500);
    }
  }
    else Serial.println("SD card correctly initialized");


  // Mandatory to begin the MTP session.
  MTP.begin();

  // Add SD Card
  MTP.addFilesystem(SD, "Kais Audio guestbook"); // choose a nice name for the SD card volume to appear in your file explorer
  Serial.println("Added SD card via MTP");
  MTPcheckInterval = MTP.storage()->get_DeltaDeviceCheckTimeMS();
    
  // Value in dB
  sgtl5000_1.micGain(20); // much lower gain is required for the AOM5024 electret capsule

  // Synchronise the Time object used in the program code with the RTC time provider.
  // See https://github.com/PaulStoffregen/Time
  setSyncProvider(getTeensy3Time);
  
  // Define a callback that will assign the correct datetime for any file system operations
  // (i.e. saving a new audio recording onto the SD card)
  FsDateTime::setCallback(dateTime);

  mode = Mode::Ready; print_mode();
}

void loop() {
  // First, read the buttons
  buttonRecord.update();
  buttonPlay.update();
  rotaryEngage.update();

  switch(mode){
    case Mode::Ready:
      lang = Lang::EN;
      // Rising edge = handset lifted
      if (buttonRecord.risingEdge()) {
        Serial.println("Handset lifted");
        mode = Mode::Prompting; print_mode();
      }
      else if(buttonPlay.fallingEdge()) {
        playLastRecording();
      }
      break;

    case Mode::Prompting:
      // Wait a second for users to put the handset to their ear
      wait(1000);
      // Play the greeting inviting them to select their language
      playWav1.play("Greeting0.wav");
      // While the message is playing (is not stopped)
      while (!playWav1.isStopped()) {
        buttonRecord.update();
        buttonPlay.update();
        rotaryEngage.update();
        // Rotary dial is used
        if (rotaryEngage.fallingEdge()) {
          Input i = dial_wait();
          print_Input(i);
          switch(i) {
            case Input::HungUp:
              playWav1.stop();
              mode = Mode::Ready; print_mode();
              return;
            case Input::PlayButtonDown:
              playWav1.stop();
              playLastRecording();
              return;
            case Input::Dial1:
              lang = Lang::EN;
              playWav1.stop();
              playWav1.play("Greeting1.wav");
              break;
            case Input::Dial2:
              lang = Lang::ES;
              playWav1.stop();
              playWav1.play("Greeting2.wav");
              break;
            case Input::Dial3:
              lang = Lang::FR;
              playWav1.stop();
              playWav1.play("Greeting3.wav");
              break;
            case Input::Dial4:
              playWav1.stop();
              playWav1.play("Greeting0.wav");
              break;
            case Input::Dial5:
              break;
            case Input::Dial6:
              playWav1.stop();
              playWav1.play("Debug1.wav");
              mode = Mode::Playing; print_mode();
              return;
            case Input::Dial7:
              playWav1.stop();
              playWav1.play("Debug2.wav");
              mode = Mode::Playing; print_mode();
              return;
            case Input::Dial8:
              playWav1.stop();
              playWav1.play("Debug3.wav");
              mode = Mode::Playing; print_mode();
              return;
            case Input::Dial9:
              playWav1.stop();
              playWav1.play("Debug4.wav");
              mode = Mode::Playing; print_mode();
              return;
            case Input::Dial0:
              playWav1.stop();
              playAllRecordings();
              return;
            case Input::PlayButtonUp:
            case Input::NoInput:
              break;
          }
        }
        // Handset is replaced
        if(buttonRecord.fallingEdge()) {
          playWav1.stop();
          mode = Mode::Ready; print_mode();
          return;
        }
        // Playback button is pressed
        if(buttonPlay.fallingEdge()) {
          playWav1.stop();
          playLastRecording();
          return;
        }
        
      }
      // Debug message
      Serial.println("Starting Recording");
      // Play the tone sound effect
      waveform1.begin(beep_volume, 440, WAVEFORM_SINE);
      wait(1250);
      waveform1.amplitude(0);
      // Start the recording function
      startRecording();
      break;

    case Mode::Recording:
      {
        bool hungUp = buttonRecord.fallingEdge();
        // Handset is replaced OR started to dial
        if(hungUp || rotaryEngage.fallingEdge()){
          // Debug log
          Serial.println("Stopping Recording");
          // Stop recording
          stopRecording();
          // Play audio tone to confirm recording has ended
          end_Beep();
          if (hungUp) {
            mode = Mode::Ready; print_mode();
          } else {
            mode = Mode::PostRecording; print_mode();
          }
        }
        else {
          continueRecording();
        }
      }
      break;

    case Mode::PostRecording:
      {
        Input i = dial_wait();
        print_Input(i);
        switch(i) {
          case Input::HungUp:
            end_Beep();
            mode = Mode::Ready; print_mode();
            return;
          case Input::Dial1:
          case Input::Dial2:
          case Input::Dial3:
          case Input::Dial4:
          case Input::Dial5:
          case Input::Dial6:
          case Input::Dial7:
          case Input::Dial8:
          case Input::Dial9:
          case Input::Dial0:
          case Input::NoInput:
            playLastRecording();
            break;
          case Input::PlayButtonDown:
          case Input::PlayButtonUp:
            break;
        }
        mode = Mode::Ready; print_mode();
      }
      break;

    case Mode::Playing: // to make compiler happy
      if(buttonRecord.fallingEdge()) {
        playWav1.stop();
        mode = Mode::Ready; print_mode();
        return;
      }
      break;  

    case Mode::Initialising: // to make compiler happy
      break;  
  }   
  
  MTP.loop();  // This is mandatory to be placed in the loop code.
}

void setMTPdeviceChecks(bool nable)
{
  if (nable)
  {
    MTP.storage()->set_DeltaDeviceCheckTimeMS(MTPcheckInterval);
    Serial.print("En");
  }
  else
  {
    MTP.storage()->set_DeltaDeviceCheckTimeMS((uint32_t) -1);
    Serial.print("Dis");
  }
  Serial.println("abled MTP storage device checks");
}
  

#if defined(INSTRUMENT_SD_WRITE)
static uint32_t worstSDwrite, printNext;
#endif // defined(INSTRUMENT_SD_WRITE)

void startRecording() {
  setMTPdeviceChecks(false); // disable MTP device checks while recording
#if defined(INSTRUMENT_SD_WRITE)
  worstSDwrite = 0;
  printNext = 0;
#endif // defined(INSTRUMENT_SD_WRITE)
  // Find the first available file number
  for (uint16_t i=0; i<9999; i++) {   
    // Format the counter as a five-digit number with leading zeroes, followed by file extension
    snprintf(filename, 15, " msg/%05d.wav", i);
    // Create if does not exist, do not open existing, write, sync after write
    if (!SD.exists(filename)) {
      break;
    }
  }
  frec = SD.open(filename, FILE_WRITE);
  Serial.println("Opened file !");
  if(frec) {
    Serial.print("Recording to ");
    Serial.println(filename);
    queue1.begin();
    mode = Mode::Recording; print_mode();
    recByteSaved = 0L;
  }
  else {
    Serial.println("Couldn't open file to record!");
  }
}

void continueRecording() {
#if defined(INSTRUMENT_SD_WRITE)
  uint32_t started = micros();
#endif // defined(INSTRUMENT_SD_WRITE)
#define NBLOX 16  
  // Check if there is data in the queue
  if (queue1.available() >= NBLOX) {
    byte buffer[NBLOX*AUDIO_BLOCK_SAMPLES*sizeof(int16_t)];
    // Fetch 2 blocks from the audio library and copy
    // into a 512 byte buffer.  The Arduino SD library
    // is most efficient when full 512 byte sector size
    // writes are used.
    for (int i=0;i<NBLOX;i++)
    {
      memcpy(buffer+i*AUDIO_BLOCK_SAMPLES*sizeof(int16_t), queue1.readBuffer(), AUDIO_BLOCK_SAMPLES*sizeof(int16_t));
      queue1.freeBuffer();
    }
    // Write all 512 bytes to the SD card
    frec.write(buffer, sizeof buffer);
    recByteSaved += sizeof buffer;
  }
  
#if defined(INSTRUMENT_SD_WRITE)
  started = micros() - started;
  if (started > worstSDwrite)
    worstSDwrite = started;

  if (millis() >= printNext)
  {
    Serial.printf("Worst write took %luus\n",worstSDwrite);
    worstSDwrite = 0;
    printNext = millis()+250;
  }
#endif // defined(INSTRUMENT_SD_WRITE)
}

void stopRecording() {
  // Stop adding any new data to the queue
  queue1.end();
  // Flush all existing remaining data from the queue
  while (queue1.available() > 0) {
    // Save to open file
    frec.write((byte*)queue1.readBuffer(), AUDIO_BLOCK_SAMPLES*sizeof(int16_t));
    queue1.freeBuffer();
    recByteSaved += AUDIO_BLOCK_SAMPLES*sizeof(int16_t);
  }
  writeOutHeader();
  // Close the file
  frec.close();
  Serial.println("Closed file");
  setMTPdeviceChecks(true); // enable MTP device checks, recording is finished
}


void playAllRecordings() {
  playWav1.play("Greeting4.wav");

  while (!playWav1.isStopped()) {
    buttonPlay.update();
    buttonRecord.update();
    // Handset is put down
    if(buttonRecord.fallingEdge()) { 
      playWav1.stop();
      mode = Mode::Ready; print_mode();
      return;
    }   
  }
  // Recording files are saved in the root directory
  File dir = SD.open("/msg/");
  
  int skips = 0;

  while (true) {
    File entry =  dir.openNextFile();
    if (!entry) {
      // no more files
      entry.close();
      end_Beep();
      break;
    }

    if (skips>0) {
      skips--;
      entry.close();
      continue;
    }

    if (strstr(entry.name(), ".wav") || strstr(entry.name(), ".WAV")) {
      Serial.print("Now playing ");
      Serial.println(entry.name());
      snprintf(filename, 15, "msg/%s",entry.name());
      entry.close();
      // Play a short beep before each message
      waveform1.amplitude(beep_volume);
      wait(750);
      waveform1.amplitude(0);
      // Play the file
      playWav1.play(filename);
      mode = Mode::Playing; print_mode();
    }
    
    while (!playWav1.isStopped()) {
      buttonPlay.update();
      buttonRecord.update();
      rotaryEngage.update();
      // Handset is put down
      if(buttonRecord.fallingEdge()) { 
        playWav1.stop();
        mode = Mode::Ready; print_mode();
        dir.close();
        return;
      }
      if (rotaryEngage.fallingEdge()) {
          Input i = dial_wait();
          print_Input(i);
          switch(i) {
            case Input::HungUp:
              playWav1.stop();
              mode = Mode::Ready; print_mode();
              dir.close();
              return;
            case Input::Dial1:
              playWav1.stop();
              skips += 0;
              break;
            case Input::Dial2:
              playWav1.stop();
              skips += 1;
              break;
            case Input::Dial3:
              playWav1.stop();
              skips += 2;
              break;
            case Input::Dial4:
              playWav1.stop();
              skips += 3;
              break;
            case Input::Dial5:
              playWav1.stop();
              skips += 4;
              break;
            case Input::Dial6:
              playWav1.stop();
              skips += 5;
              break;
            case Input::Dial7:
              playWav1.stop();
              skips += 6;
              break;
            case Input::Dial8:
              playWav1.stop();
              skips += 7;
              break;
            case Input::Dial9:
              playWav1.stop();
              skips += 8;
              break;
            case Input::Dial0:
              playWav1.stop();
              entry.close();
              dir.close();
              dir = SD.open("/msg/");
              break;
            case Input::NoInput:
            case Input::PlayButtonUp:
            case Input::PlayButtonDown:
              break;
          }
        }
    }
  }
  dir.close();
  // All files have been played
  mode = Mode::Ready; print_mode();
}

void playLastRecording() {
  // Find the first available file number
  uint16_t idx = 0; 
  for (uint16_t i=0; i<9999; i++) {
    // Format the counter as a five-digit number with leading zeroes, followed by file extension
    snprintf(filename, 15, " msg/%05d.wav", i);
    // check, if file with index i exists
    if (!SD.exists(filename)) {
     idx = i - 1;
     break;
      }
  }
  // now play file with index idx == last recorded file
  snprintf(filename, 15, " msg/%05d.wav", idx);
  Serial.println(filename);
  playWav1.play(filename);
  mode = Mode::Playing; print_mode();
  while (!playWav1.isStopped()) { // this works for playWav
    buttonPlay.update();
    buttonRecord.update();
    // Button is pressed again or handset is replaced
    if(buttonPlay.risingEdge() || buttonRecord.fallingEdge()) {
      playWav1.stop();
      mode = Mode::Ready; print_mode();
      return;
    }   
  }
      // file has been played
  mode = Mode::Ready; print_mode();  
  end_Beep();
}


// Retrieve the current time from Teensy built-in RTC
time_t getTeensy3Time(){
  return Teensy3Clock.get();
}

// Callback to assign timestamps for file system operations
void dateTime(uint16_t* date, uint16_t* time, uint8_t* ms10) {

  // Return date using FS_DATE macro to format fields.
  *date = FS_DATE(year(), month(), day());

  // Return time using FS_TIME macro to format fields.
  *time = FS_TIME(hour(), minute(), second());

  // Return low time bits in units of 10 ms.
  *ms10 = second() & 1 ? 100 : 0;
}

// Non-blocking delay, which pauses execution of main program logic,
// but while still listening for input 
void wait(unsigned int milliseconds) {
  elapsedMillis msec=0;

  while (msec <= milliseconds) {
    buttonRecord.update();
    buttonPlay.update();
    if (buttonRecord.fallingEdge()) Serial.println("Button (pin 0) Press");
    if (buttonPlay.fallingEdge()) Serial.println("Button (pin 1) Press");
    if (buttonRecord.risingEdge()) Serial.println("Button (pin 0) Release");
    if (buttonPlay.risingEdge()) Serial.println("Button (pin 1) Release");
  }
}

void writeOutHeader() { // update WAV header with final filesize/datasize

//  NumSamples = (recByteSaved*8)/bitsPerSample/numChannels;
//  Subchunk2Size = NumSamples*numChannels*bitsPerSample/8; // number of samples x number of channels x number of bytes per sample
  Subchunk2Size = recByteSaved - 42; // because we didn't make space for the header to start with! Lose 21 samples...
  ChunkSize = Subchunk2Size + 34; // was 36;
  frec.seek(0);
  frec.write("RIFF");
  byte1 = ChunkSize & 0xff;
  byte2 = (ChunkSize >> 8) & 0xff;
  byte3 = (ChunkSize >> 16) & 0xff;
  byte4 = (ChunkSize >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  frec.write("WAVE");
  frec.write("fmt ");
  byte1 = Subchunk1Size & 0xff;
  byte2 = (Subchunk1Size >> 8) & 0xff;
  byte3 = (Subchunk1Size >> 16) & 0xff;
  byte4 = (Subchunk1Size >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = AudioFormat & 0xff;
  byte2 = (AudioFormat >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = numChannels & 0xff;
  byte2 = (numChannels >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = sampleRate & 0xff;
  byte2 = (sampleRate >> 8) & 0xff;
  byte3 = (sampleRate >> 16) & 0xff;
  byte4 = (sampleRate >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = byteRate & 0xff;
  byte2 = (byteRate >> 8) & 0xff;
  byte3 = (byteRate >> 16) & 0xff;
  byte4 = (byteRate >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = blockAlign & 0xff;
  byte2 = (blockAlign >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = bitsPerSample & 0xff;
  byte2 = (bitsPerSample >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  frec.write("data");
  byte1 = Subchunk2Size & 0xff;
  byte2 = (Subchunk2Size >> 8) & 0xff;
  byte3 = (Subchunk2Size >> 16) & 0xff;
  byte4 = (Subchunk2Size >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  frec.close();
  Serial.println("header written"); 
  Serial.print("Subchunk2: "); 
  Serial.println(Subchunk2Size); 
}

void end_Beep(void) {
          waveform1.frequency(523.25);
        waveform1.amplitude(beep_volume);
        wait(250);
        waveform1.amplitude(0);
        wait(250);
        waveform1.amplitude(beep_volume);
        wait(250);
        waveform1.amplitude(0);
        wait(250);
        waveform1.amplitude(beep_volume);
        wait(250);
        waveform1.amplitude(0);
        wait(250);
        waveform1.amplitude(beep_volume);
        wait(250);
        waveform1.amplitude(0);
}

void print_mode(void) { // only for debugging
  Serial.print("Mode switched to: ");
  // Initialising, Ready, Prompting, Recording, Playing
  if(mode == Mode::Ready)           Serial.println(" Ready");
  else if(mode == Mode::Prompting)  Serial.println(" Prompting");
  else if(mode == Mode::Recording)  Serial.println(" Recording");
  else if(mode == Mode::PostRecording)  Serial.println(" PostRecording");
  else if(mode == Mode::Playing)    Serial.println(" Playing");
  else if(mode == Mode::Initialising)  Serial.println(" Initialising");
  else Serial.println(" Undefined");
}

void print_Input(Input i) {
  Serial.print("Input is: ");
  if (i == Input::NoInput)    Serial.println("No input");
  else if (i == Input::HungUp)    Serial.println("Hung up");
  else if (i == Input::PlayButtonDown)    Serial.println("Playback button pressed");
  else if (i == Input::PlayButtonUp)    Serial.println("Playback button released");
  else if (i == Input::Dial1)    Serial.println("Dial 1");
  else if (i == Input::Dial2)    Serial.println("Dial 2");
  else if (i == Input::Dial3)    Serial.println("Dial 3");
  else if (i == Input::Dial4)    Serial.println("Dial 4");
  else if (i == Input::Dial5)    Serial.println("Dial 5");
  else if (i == Input::Dial6)    Serial.println("Dial 6");
  else if (i == Input::Dial7)    Serial.println("Dial 7");
  else if (i == Input::Dial8)    Serial.println("Dial 8");
  else if (i == Input::Dial9)    Serial.println("Dial 9");
  else if (i == Input::Dial0)    Serial.println("Dial 0");
  else Serial.println(" Undefined");
}

// Called after starting to dial (rotaryEngage.fallingEdge is detected)
Input dial_wait() {
  Serial.println("Dial waiting");
  int rotaryNum = 0;

  // Rising edge = rotary mechanism has returned from dialled to neutral position - so while still waiting for the dial to finish
  while (!rotaryEngage.risingEdge()) {
    // First, read the buttons
    buttonRecord.update();
    buttonPlay.update();
    rotaryEngage.update();
    rotaryEncode.update();
  
    if (rotaryEngage.fallingEdge()) {
      Serial.println("Rotary Engage falling edge error!");
      return Input::NoInput;
    }

    // Counts the number dialled by counting number of connections made
    if (rotaryEncode.risingEdge()) {
      rotaryNum += 1;
    }

    if (buttonRecord.fallingEdge()) {
      return Input::HungUp;
    }

    if (buttonPlay.fallingEdge()) {
      return Input::PlayButtonDown;
    }

    if (buttonPlay.risingEdge()) {
      return Input::PlayButtonUp;
    }
  }

  if (rotaryNum <= 1) {
    return Input::NoInput;
  }

  // One more connection is made than the number on the dial
  if (rotaryNum == 2) return Input::Dial1;
  if (rotaryNum == 3) return Input::Dial2;
  if (rotaryNum == 4) return Input::Dial3;
  if (rotaryNum == 5) return Input::Dial4;
  if (rotaryNum == 6) return Input::Dial5;
  if (rotaryNum == 7) return Input::Dial6;
  if (rotaryNum == 8) return Input::Dial7;
  if (rotaryNum == 9) return Input::Dial8;
  if (rotaryNum == 10) return Input::Dial9;
  if (rotaryNum == 11) return Input::Dial0;

  if (rotaryNum > 11) {
    Serial.println("RotaryNum too high!");
    return Input::NoInput;
  }
  return Input::NoInput;
}
