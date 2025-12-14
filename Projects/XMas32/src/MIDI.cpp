#include "MIDI.h"
#include <ESPFileSystem.h>
#include <cstring>
#include <algorithm>
#include <Tracer.h>
#include <PSRAM.h>

namespace MIDI
{

bool File::parse(const uint8_t* data, size_t size)
{
    TRACE("File::parse(%p, %u)\n", data, size);

    if (!data || size < 14)
    {
        _error = "Invalid data or size too small";
        return false;
    }
    
    _dataPtr = data;
    _size = size;
    _pos = 0;
    _format = 0;
    _trackCount = 0;
    _division = 480;
    _tracks.clear();
    _error.clear();
    
    if (!parseHeader()) return false;

    TRACE("Format: %u. %u tracks. Division: %u\n", _format, _trackCount, _division);

    uint32_t globalTempo = 0;
    uint8_t globalBeatsPerBar = 0;
    
    _tracks.reserve(_trackCount);
    for (uint16_t i = 1; i <= _trackCount; i++)
    {
        Track track(_division, globalTempo, globalBeatsPerBar);
        
        if (!parseTrack(track)) return false;

        globalTempo = track._tempo;
        globalBeatsPerBar = track._beatsPerBar;
        TRACE("Tempo: %u. Beats per bar: %u\n", globalTempo, globalBeatsPerBar);
        
        if (track._name.isEmpty())
        {
            track._name = "Track #";
            track._name += i;            
        }

        _tracks.push_back(std::move(track));

        Tracer::traceFreeHeap();
    }
    
    return true;
}

bool File::load(const char* filename)
{
    Tracer tracer("File::load", filename);

    if (!SPIFFS.exists(filename))
    {
        _error = "Failed to open file";
        return false;
    }
    
    fs::File fsFile = SPIFFS.open(filename, "r");
    size_t fileSize = fsFile.size();
    uint8_t* bufferPtr = Memory::allocate<uint8_t>(fileSize);
    size_t bytesRead = fsFile.read(bufferPtr, fileSize);
    fsFile.close();
    
    bool success; 
    if (bytesRead == fileSize)
        success = parse(bufferPtr, fileSize); 
    else
    {
        _error = "Failed to read complete file";
        success = false;
    }

    free(bufferPtr);
    return success;
}

bool File::parseHeader()
{
    if (!checkBytes("MThd", 4))
    {
        _error = "Invalid MIDI header signature";
        return false;
    }
    
    uint32_t headerLength = readDWord();
    if (headerLength != 6)
    {
        _error = "Invalid MIDI header length";
        return false;
    }
    
    _format = readWord();
    if (_format > 2)
    {
        _error = "Unsupported MIDI format";
        return false;
    }
    
    _trackCount = readWord();
    _division = readWord();
    return true;
}

bool File::parseTrack(Track& outTrack)
{
    Tracer tracer("File::parseTrack");

    if (!checkBytes("MTrk", 4))
    {
        _error = "Invalid track signature";
        return false;
    }
    
    uint32_t length = readDWord();
    size_t trackEnd = _pos + length;
    if (trackEnd > _size)
    {
        _error = "Track extends beyond end of file";
        return false;
    }
    _currentTrackEnd = trackEnd;
    TRACE("Track length: %u. End: %u\n", length, trackEnd);
    
    uint8_t runningStatus = 0;
    while (_pos < trackEnd)
    {
        Event event;
        if (parseEvent(runningStatus, outTrack, event))
        {
            outTrack._events.push_back(event);
        }
    }
    
    TRACE("%u MIDI events\n", outTrack._events.size());

    _pos = trackEnd;
    _currentTrackEnd = 0;

    return true;
}

bool File::parseEvent(uint8_t& runningStatus, Track& track, Event& outEvent)
{
    uint32_t deltaTicks = readVariableLength();
    uint8_t statusByte = readByte();
    
    if (statusByte < 0x80)
    {
        statusByte = runningStatus;
        _pos--;
    }
    else
        runningStatus = statusByte;
    
    EventType eventType = static_cast<EventType>(statusByte & 0xF0);
    
    if (statusByte == 0xFF)
    {
        MetaEventType metaType = static_cast<MetaEventType>(readByte());
        uint32_t length = readVariableLength();
        TRACE("Meta event %02X. Length: %u\n", metaType, length);

        // Bound meta length to remaining bytes in track to avoid bad_alloc
        size_t remaining = (_currentTrackEnd > _pos) ? (_currentTrackEnd - _pos) : 0;
        if (length > remaining)
        {
            _error = "Meta event length exceeds track bounds";
            length = static_cast<uint32_t>(remaining);
        }

        if (metaType == MetaEventType::TrackName || ((metaType == MetaEventType::TextEvent) && track._name.isEmpty()))
        {
            std::vector<uint8_t> data(length);
            for (uint32_t i = 0; i < length; i++)
            {
                data[i] = readByte();
            }
            data.push_back(0); // Null terminator
            track._name = String(reinterpret_cast<const char*>(data.data()));
            TRACE("Track name: '%s'\n", track._name.c_str());
        }
        else if (metaType == MetaEventType::SetTempo)
        {
            if (length >= 3)
            {
                uint8_t b0 = readByte();
                uint8_t b1 = readByte();
                uint8_t b2 = readByte();
                track._tempo = (b0 << 16) | (b1 << 8) | b2;
                if (length > 3)
                    skipBytes(length - 3);
                TRACE("Tempo: %u\n", track._tempo);
            }
            else
            {
                skipBytes(length);
            }
        }
        else if (metaType == MetaEventType::TimeSignature)
        {
            if (length >= 2)
            {
                track._beatsPerBar = readByte();  // numerator
                uint8_t denominatorPower = readByte(); // denominator as power of 2
                uint8_t denominator = 1 << denominatorPower; // actual denominator = 2^denominatorPower
                if (length > 2)
                    skipBytes(length - 2);
                TRACE("Time Signature: %u/%u\n", track._beatsPerBar, denominator);
            }
            else
            {
                skipBytes(length);
            }
        }
        else
        {
            // For all other meta events, just skip the data
            skipBytes(length);
        }
        
        runningStatus = 0;
        return false;
    }
    else if (eventType == EventType::SystemExclusive)
    {
        uint32_t length = readVariableLength();
        TRACE("SysEx length: %u\n", length);

        skipBytes(length);
        
        runningStatus = 0;
        return false;
    }
    else
    {
        outEvent._deltaTicks = deltaTicks;
        outEvent._statusByte = statusByte;
        
        switch (eventType)
        {
            case EventType::NoteOff:
            case EventType::NoteOn:
            case EventType::PolyPressure:
            case EventType::ControlChange:
            case EventType::PitchBend:
                outEvent._byte1 = readByte();
                outEvent._byte2 = readByte();
                if (eventType == EventType::NoteOn && outEvent.getVelocity() == 0)
                    outEvent._statusByte = outEvent.getChannel() | static_cast<uint8_t>(EventType::NoteOff);
                break;
                
            case EventType::ProgramChange:
            case EventType::ChannelPressure:
                outEvent._byte1 = readByte();
                break;
                
            case EventType::SystemExclusive:
                // Shouldn't come here; SysEx is handled above.
                break;
        }
        
        return true;
    }
}

uint8_t File::readByte()
{
    if (_pos >= _size) return 0;
    return _dataPtr[_pos++];
}

uint16_t File::readWord()
{
    return (readByte() << 8) | readByte();
}

uint32_t File::readDWord()
{
    return (readByte() << 24) | (readByte() << 16) | (readByte() << 8) | readByte();
}

uint32_t File::readVariableLength()
{
    uint32_t value = 0;
    uint8_t byte;
    
    do
    {
        byte = readByte();
        value = (value << 7) | (byte & 0x7F);
    } while ((byte & 0x80) && _pos < _size);
    
    return value;
}

void File::skipBytes(size_t count)
{
    _pos = std::min(_pos + count, _size);
}

bool File::checkBytes(const char* expected, size_t length)
{
    if (_pos + length > _size) return false;
    
    bool match = std::memcmp(_dataPtr + _pos, expected, length) == 0;
    if (match) _pos += length;
    return match;
}

uint32_t File::getTotalEvents() const
{
    uint32_t total = 0;
    for (const Track& track : _tracks)
        total += track.getEvents().size();
    return total;
}

uint32_t File::getTotalNotes() const
{
    uint32_t count = 0;
    for (const Track& track : _tracks)
        count += track.getTotalNotes();
    return count;
}

float File::getDurationSeconds() const
{
    float maxDuration = 0;
    for (const Track& track : _tracks)
    {
        float trackDuration = track.getDurationSeconds();
        maxDuration = std::max(maxDuration, trackDuration);
    }
    return maxDuration;
}

void File::play(uint16_t trackIndex, std::function<void(const Event&)> midiEventFunc)
{
    if (trackIndex >= _tracks.size())
    {
        return;
    }

    _currentlyPlayingPtr = &_tracks[trackIndex];
    _currentlyPlayingPtr->play(midiEventFunc);
    _currentlyPlayingPtr = nullptr;
}

void File::stop()
{
    // Best-effort: indicate not playing; task deletion handled by caller.
    _currentlyPlayingPtr = nullptr;
}

void Track::play(std::function<void(const Event&)> midiEventFunc)
{
    Tracer tracer("Track::play");

    uint32_t startTime = millis();
    uint32_t absoluteTicks = 0;
    uint32_t nextMetronomeTicks = 0; // First metronome beat is immediate
    uint8_t beat = 0; // Beat counter for metronome (0 to _beatsPerBar-1)

    for (const Event& event : _events)
    {
        _playingForMs = millis() - startTime;
        absoluteTicks += event.getDeltaTicks();
        uint32_t eventTimeMs = uint32_t(ticksToMs(absoluteTicks));
        
        // Emit metronome events for any beats before this event, with proper timing
        while (nextMetronomeTicks < absoluteTicks)
        {
            uint32_t metronomeTimeMs = uint32_t(ticksToMs(nextMetronomeTicks));
            if (_playingForMs < metronomeTimeMs)
            {
                uint32_t delayMs = static_cast<uint32_t>(metronomeTimeMs - _playingForMs);
                delay(delayMs);
                _playingForMs = millis() - startTime;
            }
            Event metronomeEvent(0xF8, beat);
            midiEventFunc(metronomeEvent);
            nextMetronomeTicks += _division;
            beat = (beat + 1) % _beatsPerBar;
        }
        
        // Await the next event
        if (_playingForMs < eventTimeMs)
        {
            uint32_t delayMs = static_cast<uint32_t>(eventTimeMs - _playingForMs);
            delay(delayMs);
        }
        
        // Emit the event
        midiEventFunc(event);
    }
}

float Track::getDurationSeconds() const
{
    uint32_t absoluteTicks = 0;
    for (const Event& event : _events)
        absoluteTicks += event.getDeltaTicks();
    return ticksToMs(absoluteTicks) / 1000.0f;
}

uint32_t Track::getTotalNotes() const
{
    uint32_t count = 0;
    for (const Event& event : _events)
        if (event.getType() == EventType::NoteOn) count++;
    return count;
}

} // namespace MIDI
