#include <Audio.h>

// set SYNTH_DEBUG to enable debug logging (0=off,1=most,2=all messages)
#define SYNTH_DEBUG 2

// define MIDI channel
#define SYNTH_MIDICHANNEL 1

// inital poly mode (POLY, MONO or PORTAMENTO)
#define SYNTH_INITIALMODE POLY

// define tuning of A4 in Hz
#define SYNTH_TUNING 440

// gain at oscillator/filter input stage (1:1)
// keep low so filter does not saturate with resonance
#define GAIN_OSC 0.5

// gain in final mixer stage for polyphonic mode (4:1)
// (0.25 is the safe value but larger sounds better :) )
#define GAIN_POLY 1.
//#define GAIN_POLY 0.25

// gain in final mixer stage for monophonic modes
//#define GAIN_MONO 1.
#define GAIN_MONO 0.25

// define delay lines for modulation effects
#define DELAY_LENGTH (16*AUDIO_BLOCK_SAMPLES)
short delaylineL[DELAY_LENGTH];
short delaylineR[DELAY_LENGTH];

// audio memory
#define AMEMORY 50

// switch between USB and UART MIDI
#if defined USB_MIDI || defined USB_MIDI_SERIAL
#define SYNTH_USBMIDI
#endif

#define SYNTH_COM Serial

#include <MIDI.h>
MIDI_CREATE_DEFAULT_INSTANCE();

//////////////////////////////////////////////////////////////////////
// Data types and lookup tables
//////////////////////////////////////////////////////////////////////
struct Oscillator {
  AudioSynthWaveformModulated*  squareLFO;
  AudioSynthWaveformModulated*  saw;
  AudioSynthWaveformModulated*  squarePWM;
  AudioSynthNoiseWhite*          noise;

  AudioMixer4*                  oscMixer;
  
  AudioFilterStateVariable*     hpf;
  AudioFilterStateVariable*     lpf;

  AudioEffectEnvelope*          env;
  
  int8_t  note;
  uint8_t velocity;
};

// synth architecture in separate file
#include "TestSynthArch.h"

#define NVOICES 8
Oscillator oscs[NVOICES] = {
  { &squareLFO0, &saw0, &squarePWM0, &noise0, &oscMixer0, &hpf0, &lpf0, &env0, -1, 0},
  { &squareLFO1, &saw1, &squarePWM1, &noise1, &oscMixer1, &hpf1, &lpf1, &env1, -1, 0},
  { &squareLFO2, &saw2, &squarePWM2, &noise2, &oscMixer2, &hpf2, &lpf2, &env2, -1, 0},
  { &squareLFO3, &saw3, &squarePWM3, &noise3, &oscMixer3, &hpf3, &lpf3, &env3, -1, 0},
  { &squareLFO4, &saw4, &squarePWM4, &noise4, &oscMixer4, &hpf4, &lpf4, &env4, -1, 0},
  { &squareLFO5, &saw5, &squarePWM5, &noise5, &oscMixer5, &hpf5, &lpf5, &env5, -1, 0},
  { &squareLFO6, &saw6, &squarePWM6, &noise6, &oscMixer6, &hpf6, &lpf6, &env6, -1, 0},
  { &squareLFO7, &saw7, &squarePWM7, &noise7, &oscMixer7, &hpf7, &lpf7, &env7, -1, 0},
};

//////////////////////////////////////////////////////////////////////
// Global variables
//////////////////////////////////////////////////////////////////////
float   masterVolume   = 0.6;
//uint8_t currentProgram = WAVEFORM_SAWTOOTH;

bool  polyOn;
bool  omniOn;
bool  velocityOn;

//bool squarePWM or LFO idk

bool squareOn = true; //default to true
bool sawOn = true;
bool noiseOn = false; //start with false

bool  sustainPressed;
float channelVolume;
float panorama;
float pulseWidth; // 0.05-0.95
float pitchBend;  // -1/+1 oct
float pitchScale;
int   octCorr;

// filter
//FilterMode_t filterMode;
float filtFreq; // 20-AUDIO_SAMPLE_RATE_EXACT/2.5
float filtReso; // 0.9-5.0
float filtAtt;  // 0-1

// envelope
bool  envOn;
float envDelay;   // 0-200
float envAttack;  // 0-200
float envHold;    // 0-200
float envDecay;   // 0-200
float envSustain; // 0-1
float envRelease; // 0-200

// FX
bool  flangerOn;
int   flangerOffset;
int   flangerDepth;
float flangerFreqCoarse;
float flangerFreqFine;

// portamento
bool     portamentoOn = false;
uint16_t portamentoTime;
int8_t   portamentoDir;
float    portamentoStep;
float    portamentoPos;

//////////////////////////////////////////////////////////////////////
// Handling of sounding and pressed notes
//////////////////////////////////////////////////////////////////////
int8_t notesOn[NVOICES]      = { -1, -1, -1, -1, -1, -1, -1, -1, };
int8_t notesPressed[NVOICES] = { -1, -1, -1, -1, -1, -1, -1, -1, };

inline void notesReset(int8_t* notes) {
  memset(notes,-1,NVOICES*sizeof(int8_t));
}

inline void notesAdd(int8_t* notes, uint8_t note) {
  int8_t *end=notes+NVOICES;
  do {
    if(*notes == -1) {
      *notes = note;
      break;
    }
  } while (++notes < end);
}

inline int8_t notesDel(int8_t* notes, uint8_t note) {
  int8_t lastNote = -1;
  int8_t *pos=notes, *end=notes+NVOICES;
  while (++pos < end && *(pos-1) != note);
  if (pos-1 != notes) lastNote = *(pos-2);
  while (pos < end) {
    *(pos-1) = *pos;
    if (*pos++ == -1) break;
  }
  if (*(end-1)==note || pos == end)
      *(end-1) = -1;
  return lastNote;
}

inline bool notesFind(int8_t* notes, uint8_t note) {
  int8_t *end=notes+NVOICES;
  do {
    if (*notes == note) return true;
  } while (++notes < end);
  return false;
}

//////////////////////////////////////////////////////////////////////
// Parameter control functions
//////////////////////////////////////////////////////////////////////

//updateFilterMode(), updateFilter(), updateEnvelope(), updateEnvelopeMode(), updateFlanger(), resetAll()

//////////////////////////////////////////////////////////////////////
// Oscillator control functions
//////////////////////////////////////////////////////////////////////
inline float noteToFreq(float note) {
  // Sets all notes as an offset of A4 (#69)
//  if (portamentoOn) note = portamentoPos;     no portamento for now
//  return SYNTH_TUNING*pow(2,(note - 69)/12.+pitchBend/pitchScale+octCorr);
  return SYNTH_TUNING*pow(2,(note - 69)/12.); //no pitch bend for now
}

inline void oscOn(Oscillator& osc, int8_t note, uint8_t velocity) {
  float v = velocityOn ? velocity/127. : 1;
  if (osc.note!=note) {
    //set osc frequencies
    if (squareOn) osc.squareLFO->frequency(noteToFreq(note));
    if (sawOn) osc.saw->frequency(noteToFreq(note));
    
    notesAdd(notesOn,note);
    if (envOn && !osc.velocity) osc.env->noteOn();
    
    //turn oscillators on
    if (squareOn) osc.squareLFO->amplitude(v*channelVolume*GAIN_OSC);
    if (sawOn) osc.saw->amplitude(v*channelVolume*GAIN_OSC);
    if (noiseOn) osc.noise->amplitude(v*channelVolume*GAIN_OSC);
    
    osc.velocity = velocity;
    osc.note = note;
  } else if (velocity > osc.velocity) {
    //turn oscillators on
    if (squareOn) osc.squareLFO->amplitude(v*channelVolume*GAIN_OSC);
    if (sawOn) osc.saw->amplitude(v*channelVolume*GAIN_OSC);
    if (noiseOn) osc.noise->amplitude(v*channelVolume*GAIN_OSC);
    osc.velocity = velocity;
  }
}

inline void oscOff(Oscillator& osc) {
  if (envOn) osc.env->noteOff();
  else {
    //turn oscillators off
    if (squareOn) osc.squareLFO->amplitude(0);
    if (sawOn) osc.saw->amplitude(0);
    if (noiseOn) osc.noise->amplitude(0);
  }
  notesDel(notesOn,osc.note);
  osc.note = -1;
  osc.velocity = 0;
}

inline void allOff() {
  Oscillator *o=oscs,*end=oscs+NVOICES;
  do {
    oscOff(*o);
    o->squareLFO->amplitude(0);
    o->saw->amplitude(0);
    o->noise->amplitude(0);
    o->env->noteOff();
  } while(++o < end);
  notesReset(notesOn);
}

//////////////////////////////////////////////////////////////////////
// MIDI handlers
//////////////////////////////////////////////////////////////////////
void OnNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  if (!omniOn && channel != SYNTH_MIDICHANNEL) return;

#if SYNTH_DEBUG > 1
  SYNTH_COM.println("NoteOn");
#endif

  notesAdd(notesPressed,note);

  Oscillator *o=oscs;
  if (portamentoOn) {
    if (portamentoTime == 0 || portamentoPos < 0) {
      portamentoPos = note;
      portamentoDir = 0;
    } else if (portamentoPos > -1) {
      portamentoDir  = note > portamentoPos ? 1 : -1;
      portamentoStep = fabs(note-portamentoPos)/(portamentoTime);
    }
    *notesOn = -1;
    oscOn(*o, note, velocity);
  }
  else if (polyOn) {
    Oscillator *curOsc=0, *end=oscs+NVOICES;
    if (sustainPressed && notesFind(notesOn,note)) {
      do {
        if (o->note == note) {
          curOsc = o;
          break;
        }
      } while (++o < end);
    }
    for (o=oscs; o < end && !curOsc; ++o) {
      if (o->note < 0) {
        curOsc = o;
        break;
      }
    }
    if (!curOsc && *notesOn != -1) {
#if SYNTH_DEBUG > 0
      SYNTH_COM.println("Stealing voice");
#endif
      curOsc = OnNoteOffReal(channel,*notesOn,velocity,true);
    }
    if (!curOsc) return;
    oscOn(*curOsc, note, velocity);
  }
  else
  {
    *notesOn = -1;
    oscOn(*o, note, velocity);
  }

  return;
}

Oscillator* OnNoteOffReal(uint8_t channel, uint8_t note, uint8_t velocity, bool ignoreSustain) {
  if (!omniOn && channel != SYNTH_MIDICHANNEL) return 0;

#if SYNTH_DEBUG > 1
  SYNTH_COM.println("NoteOff");
#endif
  int8_t lastNote = notesDel(notesPressed,note);

  if (sustainPressed && !ignoreSustain) return 0;

  Oscillator *o=oscs;
  if (portamentoOn) {
    if (o->note == note) {
      if (lastNote != -1) {
        notesDel(notesOn,note);
        if (portamentoTime == 0) {
          portamentoPos = lastNote;
          portamentoDir = 0;
        } else {
          portamentoDir = lastNote > portamentoPos? 1 : -1;
          portamentoStep = fabs(lastNote-portamentoPos)/(portamentoTime);
        }
        oscOn(*o, lastNote, velocity);
      }
      else
      {
        oscOff(*o);
        portamentoPos = -1;
        portamentoDir = 0;
      }
    }
    if (oscs->note == note) {
      if (lastNote != -1) {
        notesDel(notesOn,o->note);
        oscOn(*o, lastNote, velocity);
      } else {
        oscOff(*o);
      }
    }
  }
  else if (polyOn) {
    Oscillator *end=oscs+NVOICES;
    do {
      if (o->note == note) break;
    } while (++o < end);
    if (o == end) return 0;
    oscOff(*o);
  } else {
    if (oscs->note == note) {
      if (lastNote != -1) {
        notesDel(notesOn,o->note);
        oscOn(*o, lastNote, velocity);
      } else {
        oscOff(*o);
      }
    }
  }

  return o;
}

inline void OnNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
  OnNoteOffReal(channel,note,velocity,false);
}

void OnAfterTouchPoly(uint8_t channel, uint8_t note, uint8_t value) {
#if SYNTH_DEBUG > 0
  SYNTH_COM.print("AfterTouchPoly: channel ");
  SYNTH_COM.print(channel);
  SYNTH_COM.print(", note ");
  SYNTH_COM.print(note);
  SYNTH_COM.print(", value ");
  SYNTH_COM.println(value);
#endif
}

inline void printResources( float cpu, uint8_t mem) {
  SYNTH_COM.print( "CPU Usage: ");
  SYNTH_COM.print(cpu);
  SYNTH_COM.print( "%, Memory: ");
  SYNTH_COM.println(mem);
}

//void performanceCheck() {
//  static unsigned long last = 0;
//  unsigned long now = millis();
//  if ((now-last)>1000) {
//    last = now;
//    float cpu = AudioProcessorUsageMax();
//    uint8_t mem = AudioMemoryUsageMax();
//    if( (statsMem!=mem) || fabs(statsCpu-cpu)>1) {
//      printResources( cpu, mem);
//    }
//    AudioProcessorUsageMaxReset();
//    AudioMemoryUsageMaxReset();
//    last = now;
//    statsCpu = cpu;
//    statsMem = mem;
//  }
//}

//////////////////////////////////////////////////////////////////////
// setup() and loop()
//////////////////////////////////////////////////////////////////////

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  AudioMemory(AMEMORY);
  sgtl5000_1.enable();
  sgtl5000_1.volume(masterVolume);


  usbMIDI.setHandleNoteOff(OnNoteOff);
  usbMIDI.setHandleNoteOn(OnNoteOn);
//  usbMIDI.setHandleVelocityChange(OnAfterTouchPoly);
//  usbMIDI.setHandleControlChange(OnControlChange);
//  usbMIDI.setHandlePitchChange(OnPitchChange);
//  usbMIDI.setHandleProgramChange(OnProgramChange);
//  usbMIDI.setHandleAfterTouch(OnAfterTouch);
//  usbMIDI.setHandleSysEx(OnSysEx);
  //usbMIDI.setHandleRealTimeSystem(OnRealTimeSystem);
//  usbMIDI.setHandleTimeCodeQuarterFrame(OnTimeCodeQFrame);
}

void loop() {
  // put your main code here, to run repeatedly:
  usbMIDI.read();
}
