#pragma once

#include <cstdint>
#include <vector>
#include <WString.h>
#include <memory>
#include <functional>

namespace MIDI
{

enum class EventType : uint8_t
{
    NoteOff = 0x80,
    NoteOn = 0x90,
    PolyPressure = 0xA0,
    ControlChange = 0xB0,
    ProgramChange = 0xC0,
    ChannelPressure = 0xD0,
    PitchBend = 0xE0,
    SystemExclusive = 0xF0
};

enum class MetaEventType : uint8_t
{
    SequenceNumber = 0x00,
    TextEvent = 0x01,
    Copyright = 0x02,
    TrackName = 0x03,
    InstrumentName = 0x04,
    Lyric = 0x05,
    Marker = 0x06,
    CuePoint = 0x07,
    ChannelPrefix = 0x20,
    EndOfTrack = 0x2F,
    SetTempo = 0x51,
    SMPTEOffset = 0x54,
    TimeSignature = 0x58,
    KeySignature = 0x59,
    SequencerSpecific = 0x7F
};

class Event
{
    friend class File;
    
public:
    uint32_t getDeltaTicks() const { return _deltaTicks; }
    EventType getType() const { return static_cast<EventType>(_statusByte & 0xF0); }
    uint8_t getChannel() const { return _statusByte & 0x0F; }
    uint8_t getNote() const { return _byte1; }
    uint8_t getVelocity() const { return _byte2; }
    uint8_t getController() const { return _byte1; }
    uint8_t getControllerValue() const { return _byte2; }
    uint16_t getPitchBend() const { return (_byte2 << 7) | _byte1; }

private:
    uint32_t _deltaTicks = 0;
    uint8_t _statusByte = 0;
    uint8_t _byte1 = 0;
    uint8_t _byte2 = 0;
};

// Forward declaration
class Track;

class File
{
public:
    bool parse(const uint8_t* data, size_t size);
    bool load(const char* filename);
    
    const String& getError() const { return _error; }
    
    uint16_t getFormat() const { return _format; }
    uint16_t getTrackCount() const { return _trackCount; }
    uint16_t getDivision() const { return _division; }
    const std::vector<Track>& getTracks() const { return _tracks; }
    uint32_t getTempo() const { return _tempo; }
    float getBPM() const { return 60000000.0f / (float)_tempo; }
    Track* getCurrentlyPlaying() const { return _currentlyPlayingPtr; }
    
    uint32_t getTotalEvents() const;
    uint32_t getTotalNotes() const;
    float getDurationSeconds() const;
    void play(uint16_t trackIndex, std::function<void(const Event&)> midiEventFunc);
    
    float getMillisecondsPerTick() const
    {
        return (float)_tempo / (float)_division / 1000.0f;
    }
    
    float ticksToMs(uint32_t ticks) const
    {
        return ticks * getMillisecondsPerTick();
    }
    
private:
    uint16_t _format = 0;
    uint16_t _trackCount = 0;
    uint16_t _division = 480;
    uint32_t _tempo = 500000;
    std::vector<Track> _tracks;
    Track* _currentlyPlayingPtr = nullptr;
    
    String _error;
    const uint8_t* _dataPtr = nullptr;
    size_t _size = 0;
    size_t _pos = 0;
    size_t _currentTrackEnd = 0;
    
    bool parseHeader();
    bool parseTrack(Track& outTrack);
    bool parseEvent(uint8_t& runningStatus, Track& track, Event& outEvent);
    uint8_t readByte();
    uint16_t readWord();
    uint32_t readDWord();
    uint32_t readVariableLength();
    void skipBytes(size_t count);
    bool checkBytes(const char* expected, size_t length);
    bool isEOF() const { return _pos >= _size; }
    void setError(const String& error);
};

class Track
{
    friend class File;
    
public:
    const std::vector<Event>& getEvents() const { return _events; }
    const String& getName() const { return _name; }
    uint32_t getTotalNotes() const;
    float getDurationSeconds() const;
    uint32_t getPlayingForSeconds() const { return _playingForMs / 1000; }
    
private:
    std::vector<Event> _events;
    String _name;
    File* _filePtr;
    uint32_t _playingForMs = 0;

    Track(File* file) : _filePtr(file) {}
    void play(std::function<void(const Event&)> midiEventFunc);
};

} // namespace MIDI
