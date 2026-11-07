// =============================================================================
//   chipmusic.h - a small note sequencer for Vircon32
// =============================================================================
//
//   Plays songs as note events (generated into music_data.h by
//   make_music.py) using a few short instrument samples: looping pulse and
//   triangle waveforms, plus one-shot drums. Each note picks its pitch by
//   setting the channel's playback speed, and the engine shapes its volume
//   every frame (decay toward a sustain level, then a quick release) and
//   can add vibrato. This keeps several songs in a few kilobytes instead of
//   megabytes of rendered audio.
//
//   USAGE
//     #include "music_data.h"   (first)
//     #include "chipmusic.h"
//     music_init( first_instrument_sound, first_channel );
//     music_play( MUSIC_TITLE, 0.6 );   // same track again: keeps playing
//     music_update();                   // once per frame, not while paused
//     music_stop();  music_is_playing();  music_current();
//     music_position();  music_play_at( track, volume, frame );   // resume
//
//   The engine uses up to MUSIC_VOICES consecutive channels starting at
//   first_channel; keep sound effects off those.
//
//   Instrument sounds must be in the ROM in this order, starting at
//   first_instrument_sound: pulse 12.5%, pulse 25%, pulse 50% (all looping,
//   pitched at 441 Hz), triangle (looping, 110.25 Hz), then kick, snare,
//   closed hat and open hat.
// =============================================================================

#ifndef CHIPMUSIC_H
#define CHIPMUSIC_H

#include "audio.h"
#include "math.h"

#define MUSIC_VOICES     7
#define MUS_INS_TRIANGLE 3
#define MUS_INS_DRUMS    4

// overall level: velocity 127 maps to this channel volume (before the
// volume passed to music_play); keeps the summed channels from clipping
#define MUSIC_LEVEL      0.42

#define MUS_OFF       0
#define MUS_HELD      1
#define MUS_RELEASE   2
#define MUS_ONESHOT   3

int mus_first_sound = 0;
int mus_first_channel = 0;
int mus_track = -1;
bool mus_playing = false;
int mus_time = 0;
float mus_volume = 1.0;

float[ 128 ] mus_note_speed;          // playback speed for a pulse sample, per MIDI note

int[ MUSIC_VOICES ] mus_ptr;
int[ MUSIC_VOICES ] mus_state;
int[ MUSIC_VOICES ] mus_frames_left;
int[ MUSIC_VOICES ] mus_age;
int[ MUSIC_VOICES ] mus_vib;
float[ MUSIC_VOICES ] mus_env;
float[ MUSIC_VOICES ] mus_sus;
float[ MUSIC_VOICES ] mus_k;
float[ MUSIC_VOICES ] mus_gain;
float[ MUSIC_VOICES ] mus_speed;

void music_init( int first_instrument_sound, int first_channel )
{
    int n, i;

    mus_first_sound = first_instrument_sound;
    mus_first_channel = first_channel;

    // equal temperament, relative to the 441 Hz pulse samples
    for( n = 0; n < 128; n++ )
      mus_note_speed[ n ] = pow( 2.0, ( n - 69 ) / 12.0 ) * 440.0 / 441.0;

    // the waveforms loop for as long as a note is held
    for( i = 0; i < MUS_INS_DRUMS; i++ )
    {
        select_sound( first_instrument_sound + i );
        set_sound_loop( true );
    }

    for( i = 0; i < MUSIC_VOICES; i++ ) mus_state[ i ] = MUS_OFF;
}

void music_stop_voices()
{
    int v;
    for( v = 0; v < MUSIC_VOICES; v++ )
    {
        if( mus_state[ v ] != MUS_OFF ) stop_channel( mus_first_channel + v );
        mus_state[ v ] = MUS_OFF;
        mus_ptr[ v ] = 0;
    }
}

void music_stop()
{
    music_stop_voices();
    mus_playing = false;
    mus_track = -1;
}

bool music_is_playing()
{
    return mus_playing;
}

// the track playing now, or -1
int music_current()
{
    if( !mus_playing ) return -1;
    return mus_track;
}

// frames into the current track (to resume it later with music_play_at)
int music_position()
{
    return mus_time;
}

// starts a track part way through: each voice skips to its first note at
// or after that frame (notes already sounding there are not restarted)
void music_play_at( int track, float volume, int time )
{
    int v, first, count, k;

    mus_volume = volume;
    music_stop_voices();
    mus_track = track;
    mus_playing = true;
    if( time < 0 || time >= mus_track_length[ track ] ) time = 0;
    mus_time = time;

    for( v = 0; v < MUSIC_VOICES; v++ )
    {
        first = mus_voice_first[ track * MUSIC_VOICES + v ];
        count = mus_voice_count[ track * MUSIC_VOICES + v ];
        k = 0;
        while( k < count && ( mus_events[ ( first + k ) * 2 ] & 0xFFFF ) < time ) k++;
        mus_ptr[ v ] = k;
    }
}

void music_set_volume( float volume )
{
    mus_volume = volume;
}

// starts a track; asking for the one already playing just sets the volume
void music_play( int track, float volume )
{
    mus_volume = volume;
    if( track == mus_track && mus_playing ) return;

    music_stop_voices();
    mus_track = track;
    mus_time = 0;
    mus_playing = true;
}

void mus_trigger( int v, int w0, int w1 )
{
    int channel = mus_first_channel + v;
    int note = w1 & 127;
    int velocity = ( w1 >> 7 ) & 127;
    int instrument = ( w1 >> 14 ) & 7;
    int decay = ( w1 >> 17 ) & 15;
    int sustain = ( w1 >> 21 ) & 15;
    float speed = 1.0;

    mus_gain[ v ] = velocity / 127.0 * MUSIC_LEVEL;

    if( instrument < MUS_INS_DRUMS )
    {
        speed = mus_note_speed[ note ];
        if( instrument == MUS_INS_TRIANGLE ) speed *= 4.0;     // 110.25 Hz sample

        mus_state[ v ] = MUS_HELD;
        mus_frames_left[ v ] = ( w0 >> 16 ) & 0x7FFF;
        mus_age[ v ] = 0;
        mus_env[ v ] = 1.0;
        mus_sus[ v ] = sustain / 15.0;
        mus_k[ v ] = mus_decay_k[ decay ];
        mus_vib[ v ] = ( w1 >> 25 ) & 3;
        mus_speed[ v ] = speed;
    }
    else mus_state[ v ] = MUS_ONESHOT;

    // a sound can only be assigned to a stopped channel
    stop_channel( channel );
    select_channel( channel );
    set_channel_speed( speed );
    set_channel_volume( mus_gain[ v ] * mus_volume );
    play_sound_in_channel( mus_first_sound + instrument, channel );
}

// advances the song by one frame
void music_update()
{
    int v, index, w0, channel;
    float env, depth, phase;
    bool any_sound = false;

    if( !mus_playing ) return;

    for( v = 0; v < MUSIC_VOICES; v++ )
    {
        channel = mus_first_channel + v;

        // start this voice's next note when its frame comes
        if( mus_ptr[ v ] < mus_voice_count[ mus_track * MUSIC_VOICES + v ] )
        {
            index = ( mus_voice_first[ mus_track * MUSIC_VOICES + v ] + mus_ptr[ v ] ) * 2;
            w0 = mus_events[ index ];

            if( ( w0 & 0xFFFF ) == mus_time )
            {
                mus_trigger( v, w0, mus_events[ index + 1 ] );
                mus_ptr[ v ]++;
                any_sound = true;
                continue;
            }
        }

        // shape the notes already sounding
        if( mus_state[ v ] == MUS_HELD )
        {
            if( mus_k[ v ] > 0 )
              mus_env[ v ] = mus_sus[ v ] + ( mus_env[ v ] - mus_sus[ v ] ) * mus_k[ v ];

            mus_frames_left[ v ]--;
            if( mus_frames_left[ v ] <= 0 ) mus_state[ v ] = MUS_RELEASE;
        }
        else if( mus_state[ v ] == MUS_RELEASE )
        {
            mus_env[ v ] *= 0.35;
            if( mus_env[ v ] < 0.01 )
            {
                stop_channel( channel );
                mus_state[ v ] = MUS_OFF;
            }
        }

        if( mus_state[ v ] == MUS_HELD || mus_state[ v ] == MUS_RELEASE )
        {
            any_sound = true;
            select_channel( channel );
            set_channel_volume( mus_env[ v ] * mus_gain[ v ] * mus_volume );

            // delayed vibrato: fades in after 9 frames, 5.5 cycles per second
            mus_age[ v ]++;
            if( mus_vib[ v ] > 0 && mus_age[ v ] > 9 )
            {
                depth = mus_vibrato_depth[ mus_vib[ v ] ] * fmin( 1.0, ( mus_age[ v ] - 9 ) / 12.0 );
                phase = mus_age[ v ] * ( 6.2831853 * 5.5 / 60.0 );
                set_channel_speed( mus_speed[ v ] * ( 1.0 + depth * sin( phase ) ) );
            }
        }
        else if( mus_state[ v ] == MUS_ONESHOT ) any_sound = true;
    }

    mus_time++;

    if( mus_time >= mus_track_length[ mus_track ] )
    {
        if( mus_track_loops[ mus_track ] )
        {
            mus_time = 0;
            for( v = 0; v < MUSIC_VOICES; v++ ) mus_ptr[ v ] = 0;
        }
        else
        {
            // a one-shot track (like a jingle) ends here
            music_stop();
        }
    }
}

#endif
