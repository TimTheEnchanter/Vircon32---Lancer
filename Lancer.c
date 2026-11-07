// =============================================================================
//   LANCER - a trench-run space shooter for Vircon32
// =============================================================================
//
//   Each wave you strike at an enemy dreadnought in three phases:
//     1. SPACE    - interceptors swoop at you, hurling fireballs
//     2. HULL     - a low pass over the hull, shooting its gun towers
//     3. TRENCH   - a dive down a trench in the hull, dodging catwalks and
//                   turret fire, to hit the reactor vent at the far end
//   Hit the vent and the dreadnought blows; miss and you go around again.
//
//   Controls: the D-pad moves the crosshair (and steers you over the hull
//   and in the trench), B or A fires both lasers, Start pauses.
//
//   Every game records its inputs; with the same seed they replay it
//   exactly, which is how the attract demo works.
//
//   Drawing: vector.h's camera stays fixed at the origin looking along +z;
//   everything else is placed relative to it (x right, y down, z ahead).
// =============================================================================

#define VEC_MAX_LINES 1000
#define LANCER_VERSION "V1.0"

#include "video.h"
#include "math.h"
#include "time.h"
#include "input.h"
#include "audio.h"
#include "misc.h"
#include "memcard.h"
#include "vector.h"
#include "music_data.h"
#include "chipmusic.h"

// -----------------------------------------------------------------------------
//   SOUND
// -----------------------------------------------------------------------------

#define SND_LASER        0
#define SND_EXPLODE      1
#define SND_FIREBALL     2
#define SND_FB_POP       3
#define SND_SHIELD       4
#define SND_BIG_BOOM     5
#define SND_BANNER       6
#define SND_BEEP         7
#define SND_TOWER        8
#define SND_HUM          9
#define SND_START        10
#define SND_GAME_OVER    11
#define SND_EXTRA        12
#define SND_INSTRUMENTS  13        // the music's instrument samples follow (see make_music.py)

#define CH_HUM         0
#define CH_FIRST_SFX   1
#define CH_MUSIC       9          // music uses channels 9-15; effects use 1-8

// player options (saved on the memory card)
#define DIFF_NORMAL  0
#define DIFF_EASY    1
#define DIFF_HARD    2
int opt_sfx = 10;                     // 0-10
int opt_music = 10;                   // 0-10
int opt_difficulty = DIFF_NORMAL;
bool opt_shake = true;
bool opt_flash = true;

float sfx_level()
{
    return opt_sfx / 10.0;
}

int next_sfx_channel = CH_FIRST_SFX;
bool hum_on = false;
bool sfx_muted = false;               // the attract demo plays silently

void sfx_pitched( int sound_id, float volume, float speed )
{
    if( sfx_muted ) return;
    stop_channel( next_sfx_channel );          // a busy channel ignores new sounds
    select_channel( next_sfx_channel );
    set_channel_volume( volume * sfx_level() );
    set_channel_speed( speed );
    play_sound_in_channel( sound_id, next_sfx_channel );
    next_sfx_channel++;
    if( next_sfx_channel >= CH_MUSIC ) next_sfx_channel = CH_FIRST_SFX;
}

void sfx( int sound_id, float volume )
{
    sfx_pitched( sound_id, volume, 1.0 );
}

// -----------------------------------------------------------------------------
//   MUSIC
// -----------------------------------------------------------------------------

float music_base = 0.5;               // volume the current track asked for

void play_track( int track, float volume )
{
    music_base = volume;
    music_play( track, volume * opt_music / 10.0 );
}

void apply_music_volume()
{
    music_set_volume( music_base * opt_music / 10.0 );
}

void update_hum( bool running, float pitch )
{
    if( sfx_muted ) running = false;
    if( running && !hum_on )
    {
        select_channel( CH_HUM );
        set_channel_volume( 0.22 );
        play_sound_in_channel( SND_HUM, CH_HUM );
        hum_on = true;
    }
    if( !running && hum_on )
    {
        stop_channel( CH_HUM );
        hum_on = false;
    }
    if( hum_on )
    {
        select_channel( CH_HUM );
        set_channel_volume( 0.22 * sfx_level() );
        set_channel_speed( pitch );
    }
}

// -----------------------------------------------------------------------------
//   RANDOM NUMBERS: gameplay (seeded per game) and effects (drawing only)
// -----------------------------------------------------------------------------

int rng_state = 1;
int fx_state = 1;

int xorshift( int x )
{
    x = x ^ ( x << 13 );
    x = x ^ ( ( x >> 17 ) & 0x7FFF );
    x = x ^ ( x << 5 );
    return x;
}

int grand()
{
    rng_state = xorshift( rng_state );
    return rng_state & 0x7FFFFFFF;
}

float frand_range( float a, float b )
{
    return a + ( b - a ) * ( grand() % 10000 ) / 10000.0;
}

float fx_range( float a, float b )
{
    fx_state = xorshift( fx_state );
    return a + ( b - a ) * ( ( fx_state & 0x7FFFFFFF ) % 10000 ) / 10000.0;
}

// atan2 that tolerates ( 0, 0 ): the Vircon32 CPU halts on that
float safe_atan2( float y, float x )
{
    if( x == 0 && y == 0 ) return 0;
    return atan2( y, x );
}

float clampf( float v, float lo, float hi )
{
    if( v < lo ) return lo;
    if( v > hi ) return hi;
    return v;
}

// -----------------------------------------------------------------------------
//   INPUT: game logic only sees these bits (ready for recorded replays)
// -----------------------------------------------------------------------------

#define IN_LEFT    1
#define IN_RIGHT   2
#define IN_UP      4
#define IN_DOWN    8
#define IN_FIRE    16

int in_bits = 0;
bool input_lock = false;          // after a menu: ignore held buttons until released

int read_gamepad_bits()
{
    int b = 0;
    if( gamepad_left() > 0 ) b = b | IN_LEFT;
    if( gamepad_right() > 0 ) b = b | IN_RIGHT;
    if( gamepad_up() > 0 ) b = b | IN_UP;
    if( gamepad_down() > 0 ) b = b | IN_DOWN;
    if( gamepad_button_b() > 0 || gamepad_button_a() > 0 ) b = b | IN_FIRE;

    if( input_lock )
    {
        if( ( b & IN_FIRE ) == 0 ) input_lock = false;
        else b = b & ~IN_FIRE;
    }
    return b;
}

// every game records its inputs, 6 bits a frame, 5 frames a word, for the
// first 60 seconds; replaying the same seed and inputs reproduces the game
#define REC_MAX_FRAMES  3600
#define REC_WORDS       720
#define DEMO_MAX_PLAY   2700      // the attract demo plays 45 s at most
#define DEMO_MIN_FRAMES 600       // shorter games never become the demo

int[ REC_WORDS ] rec_data;
int rec_frames = 0;
int game_seed = 1;

bool demo_mode = false;
int demo_pos = 0;
int[ REC_WORDS ] demo_data;
int demo_seed = 1;
int demo_frames = 0;
int demo_score = 0;
int demo_difficulty = 0;

void rec_add( int bits )
{
    if( rec_frames >= REC_MAX_FRAMES ) return;
    rec_data[ rec_frames / 5 ] = rec_data[ rec_frames / 5 ] | ( bits << ( ( rec_frames % 5 ) * 6 ) );
    rec_frames++;
}

int rec_get( int* data, int pos )
{
    if( pos >= REC_MAX_FRAMES ) return 0;
    return ( data[ pos / 5 ] >> ( ( pos % 5 ) * 6 ) ) & 63;
}

// -----------------------------------------------------------------------------
//   GAME STATE
// -----------------------------------------------------------------------------

#define STATE_TITLE     0
#define STATE_PLAYING   1
#define STATE_GAMEOVER  2
#define STATE_DEMO      3         // attract mode: replaying a recorded game
#define STATE_ENTRY     4         // entering initials
#define STATE_OPTIONS   5
#define STATE_MODES     6         // choosing a mode

#define PH_SPACE    0
#define PH_HULL     1
#define PH_TRENCH   2
#define PH_BOOM     3             // the dreadnought explodes
#define PH_CORE     4             // secret: inside the dreadnought

#define MAX_SHIELDS 8             // easy; normal has 6 and hard 4

int state = STATE_TITLE;
int state_timer = 0;
int frame = 0;
int game_frame = 0;
int anim = 0;                     // animation clock: game-relative in play
bool paused = false;

int score = 0;
int high_score = 100000;
int save_message = 0;
int wave = 1;
int shields = 6;
int max_shields = 6;              // this game's full complement (by difficulty)

// difficulty of the game being played (a demo uses the one it was recorded on)
int game_difficulty = DIFF_NORMAL;

int diff_pick( int easy, int normal, int hard )
{
    if( game_difficulty == DIFF_EASY ) return easy;
    if( game_difficulty == DIFF_HARD ) return hard;
    return normal;
}

float diff_pickf( float easy, float normal, float hard )
{
    if( game_difficulty == DIFF_EASY ) return easy;
    if( game_difficulty == DIFF_HARD ) return hard;
    return normal;
}

bool options_from_pause = false;

bool in_game()
{
    return state == STATE_PLAYING || state == STATE_DEMO
           || ( state == STATE_OPTIONS && options_from_pause );
}
int invuln = 0;                   // frames of grace after a hit
bool p_dead = false;
int dead_timer = 0;

int phase = PH_SPACE;
int phase_timer = 0;
int* banner_text;
int* banner_sub;
int banner_timer = 0;

float shake = 0;
float flash = 0;
int flash_color = 0xFF4040FF;

void add_shake( float amount )
{
    if( opt_shake && amount > shake ) shake = amount;
}

void add_flash( float amount, int color )
{
    if( !opt_flash ) return;
    if( amount > flash ) flash = amount;
    flash_color = color;
}

// -----------------------------------------------------------------------------
//   DREADNOUGHT CLASSES: each wave's target, in rotation
// -----------------------------------------------------------------------------

#define CLASS_LANCE    0          // the original: long and lean
#define CLASS_CARRIER  1          // broad; launches bombers
#define CLASS_BASTION  2          // heavily defended: generators and laser gates

int dn_class()
{
    return ( wave - 1 ) % 3;
}

int* class_name()
{
    if( dn_class() == CLASS_CARRIER ) return "CARRIER CLASS DREADNOUGHT";
    if( dn_class() == CLASS_BASTION ) return "BASTION CLASS DREADNOUGHT";
    return "LANCE CLASS DREADNOUGHT";
}

// the hull's colours: bright lines, dim lines, raised plates
int class_core()
{
    if( dn_class() == CLASS_CARRIER ) return 0xFFE0B048;
    if( dn_class() == CLASS_BASTION ) return 0xFF3890E8;
    return 0xFF50C050;
}

int class_dim()
{
    if( dn_class() == CLASS_CARRIER ) return 0xFFB08838;
    if( dn_class() == CLASS_BASTION ) return 0xFF2870B8;
    return 0xFF50A050;
}

// -----------------------------------------------------------------------------
//   HIGH SCORE TABLE
// -----------------------------------------------------------------------------

#define HS_COUNT 5

int[ HS_COUNT ] hs_score;
int[ HS_COUNT ] hs_name;              // 3 characters, 8 bits each; difficulty in the top byte

int pack_name( int* text )
{
    return text[ 0 ] | ( text[ 1 ] << 8 ) | ( text[ 2 ] << 16 );
}

void unpack_name( int name, int* text )
{
    text[ 0 ] = name & 255;
    text[ 1 ] = ( name >> 8 ) & 255;
    text[ 2 ] = ( name >> 16 ) & 255;
    text[ 3 ] = 0;
}

void init_table()
{
    hs_score[ 0 ] = 100000; hs_name[ 0 ] = pack_name( "ACE" );
    hs_score[ 1 ] = 75000;  hs_name[ 1 ] = pack_name( "LNC" );
    hs_score[ 2 ] = 50000;  hs_name[ 2 ] = pack_name( "VNT" );
    hs_score[ 3 ] = 30000;  hs_name[ 3 ] = pack_name( "RED" );
    hs_score[ 4 ] = 15000;  hs_name[ 4 ] = pack_name( "JET" );
}

int table_rank( int value )
{
    int i;
    for( i = 0; i < HS_COUNT; i++ )
      if( value > hs_score[ i ] ) return i;
    return -1;
}

void table_insert( int value, int name, int rank )
{
    int i;
    if( rank < 0 || rank >= HS_COUNT ) return;
    for( i = HS_COUNT - 1; i > rank; i-- )
    {
        hs_score[ i ] = hs_score[ i - 1 ];
        hs_name[ i ] = hs_name[ i - 1 ];
    }
    hs_score[ rank ] = value;
    hs_name[ rank ] = name;
}

void table_merge( int value, int name )
{
    int i;
    for( i = 0; i < HS_COUNT; i++ )
      if( hs_score[ i ] == value && hs_name[ i ] == name ) return;
    table_insert( value, name, table_rank( value ) );
}

// -----------------------------------------------------------------------------
//   PILOT RECORD, MODES AND DISPLAYS
// -----------------------------------------------------------------------------

#define FEATS            8
#define FEAT_DREADNOUGHT 0
#define FEAT_FLAWLESS    1
#define FEAT_SWEEP       2
#define FEAT_BOMBERS     3
#define FEAT_FLEET       4
#define FEAT_ACE         5
#define FEAT_INSTINCT    6        // hidden
#define FEAT_CORE        7        // hidden

#define MODES          4
#define MODE_STANDARD  0
#define MODE_TRENCH    1
#define MODE_TIME      2
#define MODE_LAST      3

#define DISPLAYS       5

int feats = 0;                    // one bit per feat ever achieved
bool progress_dirty = false;      // something new to save
int[ MODES ] mode_best;
int[ MODES ] mode_name;
int game_mode = MODE_STANDARD;
int chosen_mode = MODE_STANDARD;
int opt_display = 0;

bool has_feat( int f )
{
    return ( feats & ( 1 << f ) ) != 0;
}

int* feat_title( int f )
{
    if( f == FEAT_DREADNOUGHT ) return "DREADNOUGHT DOWN";
    if( f == FEAT_FLAWLESS ) return "FLAWLESS WAVE";
    if( f == FEAT_SWEEP ) return "CLEAN SWEEP";
    if( f == FEAT_BOMBERS ) return "BOMBER ACE";
    if( f == FEAT_FLEET ) return "FLEET BREAKER";
    if( f == FEAT_ACE ) return "ACE PILOT";
    if( f == FEAT_INSTINCT ) return "TRUST YOUR INSTINCTS";
    return "CORE BREACH";
}

int* feat_about( int f )
{
    if( f == FEAT_DREADNOUGHT ) return "DESTROY A DREADNOUGHT";
    if( f == FEAT_FLAWLESS ) return "FINISH A WAVE WITHOUT LOSING A SHIELD";
    if( f == FEAT_SWEEP ) return "DESTROY EVERY TURRET IN A TRENCH RUN";
    if( f == FEAT_BOMBERS ) return "SHOOT DOWN 5 BOMBERS IN ONE GAME";
    if( f == FEAT_FLEET ) return "DESTROY 3 DREADNOUGHTS IN ONE STANDARD GAME";
    if( f == FEAT_ACE ) return "SCORE 200000 IN ONE STANDARD GAME";
    if( f == FEAT_INSTINCT ) return "HIT THE VENT WITH YOUR ONLY SHOT IN THE TRENCH";
    return "SHOOT EVERY ENERGY CELL IN THE CORE";
}

int* feat_reward( int f )
{
    if( f == FEAT_DREADNOUGHT ) return "UNLOCKS TRENCH RUN";
    if( f == FEAT_FLAWLESS ) return "UNLOCKS TIME ATTACK";
    if( f == FEAT_SWEEP ) return "UNLOCKS AMBER DISPLAY";
    if( f == FEAT_BOMBERS ) return "UNLOCKS GREEN DISPLAY";
    if( f == FEAT_FLEET ) return "UNLOCKS LAST PILOT";
    if( f == FEAT_ACE ) return "UNLOCKS WHITE DISPLAY";
    if( f == FEAT_INSTINCT ) return "OPENS THE WAY INSIDE";
    return "UNLOCKS SPECTRUM DISPLAY";
}

bool feat_hidden( int f )
{
    return f == FEAT_INSTINCT || f == FEAT_CORE;
}

bool mode_unlocked( int m )
{
    if( m == MODE_TRENCH ) return has_feat( FEAT_DREADNOUGHT );
    if( m == MODE_TIME ) return has_feat( FEAT_FLAWLESS );
    if( m == MODE_LAST ) return has_feat( FEAT_FLEET );
    return true;
}

bool any_mode_unlocked()
{
    return mode_unlocked( MODE_TRENCH ) || mode_unlocked( MODE_TIME ) || mode_unlocked( MODE_LAST );
}

int* mode_title( int m )
{
    if( m == MODE_TRENCH ) return "TRENCH RUN";
    if( m == MODE_TIME ) return "TIME ATTACK";
    if( m == MODE_LAST ) return "LAST PILOT";
    return "STANDARD";
}

int* mode_about( int m )
{
    if( m == MODE_TRENCH ) return "TRENCH AFTER TRENCH - 3 SHIELDS - ONE BACK PER VENT";
    if( m == MODE_TIME ) return "3 MINUTES - A HIT COSTS 10 SECONDS - A VENT GIVES 30";
    if( m == MODE_LAST ) return "ONE SHIELD - NO REPAIRS - DOUBLE POINTS";
    return "THE FULL ATTACK - WAVE AFTER WAVE";
}

int* mode_hint( int m )
{
    if( m == MODE_TRENCH ) return "LOCKED - DESTROY A DREADNOUGHT";
    if( m == MODE_TIME ) return "LOCKED - FINISH A WAVE WITHOUT LOSING A SHIELD";
    return "LOCKED - DESTROY 3 DREADNOUGHTS IN ONE GAME";
}

bool display_unlocked( int d )
{
    if( d == 1 ) return has_feat( FEAT_SWEEP );
    if( d == 2 ) return has_feat( FEAT_BOMBERS );
    if( d == 3 ) return has_feat( FEAT_ACE );
    if( d == 4 ) return has_feat( FEAT_CORE );
    return true;
}

int* display_name( int d )
{
    if( d == 1 ) return "AMBER";
    if( d == 2 ) return "GREEN";
    if( d == 3 ) return "WHITE";
    if( d == 4 ) return "SPECTRUM";
    return "COLOUR";
}

// feats found this game, waiting to be announced
int toast_pending = 0;
int toast_feat = -1;
int toast_timer = 0;

void award_feat( int f )
{
    if( demo_mode || has_feat( f ) ) return;
    feats = feats | ( 1 << f );
    progress_dirty = true;
    toast_pending = toast_pending | ( 1 << f );
}

void update_toast()
{
    int f;
    if( toast_timer > 0 )
    {
        toast_timer--;
        return;
    }
    toast_feat = -1;
    if( toast_pending == 0 ) return;
    // announced once the fighting resumes: not over the blast or in the core
    if( phase == PH_BOOM || phase == PH_CORE || p_dead ) return;
    for( f = 0; f < FEATS; f++ )
      if( toast_pending & ( 1 << f ) ) break;
    toast_pending = toast_pending ^ ( 1 << f );
    toast_feat = f;
    toast_timer = 220;
    sfx( SND_EXTRA, 0.5 );
}

// -----------------------------------------------------------------------------
//   MEMORY CARD
// -----------------------------------------------------------------------------
//   Same layout as the other games (word offsets):
//     0-19 signature   20 format version (1)   21-25 scores   26-30 names
//     31-33 demo seed, length, score   34 checksum of 21-33
//     35 checksum of the demo   36 options   37 checksum of 36 and 38
//     38 demo difficulty, plus the rules version it was recorded under << 24
//     40-759 demo recording (REC_WORDS words)
//     760-768 pilot record: feats, three mode bests, three mode names, a
//             spare, and a checksum of the eight
//     options word: music 0-3, effects 4-7, difficulty 8-9, shake 10,
//             flash 11, marks 12-13, display 14-16
//   A card belonging to another game is never written to.

#define CARD_NONE      0
#define CARD_OURS      1
#define CARD_EMPTY     2
#define CARD_OTHER     3

#define SAVE_OFFSET     20
#define SAVE_HEADER     16
#define SAVE_EXTRA_AT   36
#define SAVE_DEMO_AT    40
#define SAVE_PROGRESS_AT 760
#define PROGRESS_WORDS  9
#define SAVE_VERSION    1
#define SAVE_KEY        0x4B1D5E23
#define DEMO_RULES      3         // bump when a gameplay change would make old demos replay differently

#define SAVE_MSG_NONE      0
#define SAVE_MSG_SAVED     1
#define SAVE_MSG_NO_CARD   2
#define SAVE_MSG_OTHER     3

int* save_signature_text = "LANCER TRENCH RUN V1";   // exactly 20 characters
int card_state = CARD_NONE;
bool card_loaded = false;
int[ SAVE_HEADER ] save_header;
int[ 3 ] save_extra;
int[ REC_WORDS ] card_demo;
int[ PROGRESS_WORDS ] save_progress;

int checksum( int* data, int count )
{
    int h = SAVE_KEY, i;
    for( i = 0; i < count; i++ )
      h = ( ( h << 5 ) | ( ( h >> 27 ) & 31 ) ) ^ data[ i ];
    return h;
}

int pack_options()
{
    int f = 0, h = 0;
    if( opt_shake ) f = 1;
    if( opt_flash ) h = 1;
    return opt_music | ( opt_sfx << 4 ) | ( opt_difficulty << 8 ) | ( f << 10 ) | ( h << 11 ) | ( 1 << 12 ) | ( 1 << 13 )
           | ( opt_display << 14 );
}

void unpack_options( int word )
{
    opt_sfx = min( ( word >> 4 ) & 15, 10 );
    opt_music = 10;                       // cards from before 0.5 have no music level
    if( ( word >> 13 ) & 1 ) opt_music = min( word & 15, 10 );
    opt_difficulty = ( word >> 8 ) & 3;
    if( opt_difficulty > DIFF_HARD ) opt_difficulty = DIFF_NORMAL;
    opt_shake = ( ( word >> 10 ) & 1 ) != 0;
    opt_flash = ( ( word >> 11 ) & 1 ) != 0;
    opt_display = ( word >> 14 ) & 7;
    if( opt_display >= DISPLAYS ) opt_display = 0;
}

int extra_checksum()
{
    int[ 2 ] w;
    w[ 0 ] = save_extra[ 0 ];
    w[ 1 ] = save_extra[ 2 ];
    return checksum( w, 2 );
}

void load_save()
{
    int i;
    bool extra_ok;

    card_read_data( save_header, SAVE_OFFSET, SAVE_HEADER );
    if( save_header[ 0 ] != SAVE_VERSION ) return;
    if( save_header[ 14 ] != checksum( &save_header[ 1 ], 13 ) ) return;

    card_read_data( save_extra, SAVE_EXTRA_AT, 3 );
    extra_ok = ( save_extra[ 1 ] == extra_checksum() && ( save_extra[ 0 ] & ( 1 << 12 ) ) );
    if( extra_ok ) unpack_options( save_extra[ 0 ] );

    for( i = 0; i < HS_COUNT; i++ )
      table_merge( save_header[ 1 + i ], save_header[ 6 + i ] );

    // the pilot record: feats add up, bests keep the higher
    card_read_data( save_progress, SAVE_PROGRESS_AT, PROGRESS_WORDS );
    if( save_progress[ 8 ] == checksum( save_progress, 8 ) )
    {
        feats = feats | save_progress[ 0 ];
        for( i = 1; i < MODES; i++ )
          if( save_progress[ i ] > mode_best[ i ] )
          {
              mode_best[ i ] = save_progress[ i ];
              mode_name[ i ] = save_progress[ 3 + i ];
          }
    }
    if( !display_unlocked( opt_display ) ) opt_display = 0;

    // a demo recorded under other rules would no longer replay the same
    if( extra_ok && ( ( save_extra[ 2 ] >> 24 ) & 15 ) == DEMO_RULES
        && save_header[ 13 ] > demo_score && save_header[ 12 ] > 0 && save_header[ 12 ] <= REC_MAX_FRAMES )
    {
        card_read_data( card_demo, SAVE_DEMO_AT, REC_WORDS );
        if( save_header[ 15 ] == checksum( card_demo, REC_WORDS ) )
        {
            memcpy( demo_data, card_demo, REC_WORDS );
            demo_seed = save_header[ 11 ];
            demo_frames = save_header[ 12 ];
            demo_score = save_header[ 13 ];
            demo_difficulty = save_extra[ 2 ] & 3;
        }
    }
}

void refresh_card()
{
    if( !card_is_connected() )
    {
        card_state = CARD_NONE;
        card_loaded = false;
        return;
    }

    if( card_signature_matches( (game_signature*)save_signature_text ) ) card_state = CARD_OURS;
    else if( card_is_empty() ) card_state = CARD_EMPTY;
    else card_state = CARD_OTHER;

    if( card_state == CARD_OURS && !card_loaded )
    {
        load_save();
        high_score = hs_score[ 0 ];
    }
    card_loaded = ( card_state != CARD_NONE );
}

int save_all()
{
    int i;
    refresh_card();

    if( card_state == CARD_NONE ) return SAVE_MSG_NO_CARD;
    if( card_state == CARD_OTHER ) return SAVE_MSG_OTHER;
    if( card_state == CARD_EMPTY ) card_write_signature( (game_signature*)save_signature_text );

    save_header[ 0 ] = SAVE_VERSION;
    for( i = 0; i < HS_COUNT; i++ )
    {
        save_header[ 1 + i ] = hs_score[ i ];
        save_header[ 6 + i ] = hs_name[ i ];
    }
    save_header[ 11 ] = demo_seed;
    save_header[ 12 ] = demo_frames;
    save_header[ 13 ] = demo_score;
    save_header[ 14 ] = checksum( &save_header[ 1 ], 13 );
    save_header[ 15 ] = checksum( demo_data, REC_WORDS );

    card_write_data( demo_data, SAVE_DEMO_AT, REC_WORDS );
    card_write_data( save_header, SAVE_OFFSET, SAVE_HEADER );

    save_extra[ 0 ] = pack_options();
    save_extra[ 2 ] = demo_difficulty | ( DEMO_RULES << 24 );
    save_extra[ 1 ] = extra_checksum();
    card_write_data( save_extra, SAVE_EXTRA_AT, 3 );

    save_progress[ 0 ] = feats;
    for( i = 1; i < MODES; i++ )
    {
        save_progress[ i ] = mode_best[ i ];
        save_progress[ 3 + i ] = mode_name[ i ];
    }
    save_progress[ 7 ] = 0;
    save_progress[ 8 ] = checksum( save_progress, 8 );
    card_write_data( save_progress, SAVE_PROGRESS_AT, PROGRESS_WORDS );
    progress_dirty = false;

    card_state = CARD_OURS;
    return SAVE_MSG_SAVED;
}

#include "demo_builtin.h"

void load_builtin_demo()
{
    demo_frames = 0;
    demo_score = 0;
    if( builtin_demo_frames <= 0 ) return;
    memcpy( demo_data, builtin_demo_data, REC_WORDS );
    demo_seed = builtin_demo_seed;
    demo_frames = builtin_demo_frames;
    demo_score = builtin_demo_score;
    demo_difficulty = DIFF_NORMAL;
}

// -----------------------------------------------------------------------------
//   CAMERA AND THE SHIP
// -----------------------------------------------------------------------------

#define SCREEN_CX  320
#define SCREEN_CY  180
#define FOCAL      420.0
#define FAR        420.0

float ship_x = 0, ship_y = 0;     // the ship's place over the hull / in the trench
float aim_x = 320, aim_y = 180;   // the crosshair on screen
float aim_vx = 0, aim_vy = 0;
float turn_x = 0, turn_y = 0;     // in space, aiming off-centre slowly turns the view
float scroll = 0;                 // how fast the world comes at you this frame

float view_dy = 0;                // the view's pitch in a transition, as a shift of the screen centre

void set_view()
{
    vec_camera( SCREEN_CX, SCREEN_CY + view_dy, FOCAL, 0, 0, 0 );
    vec_near = 0.5;
}

// -----------------------------------------------------------------------------
//   MODELS: vertices (x right, y down so negative is up, z forward) and edges
// -----------------------------------------------------------------------------

#define MAX_MODEL_VERTS 24

// interceptor: a dart-shaped body with forward-swept wings
#define FIGHTER_VERTS 14
#define FIGHTER_EDGES 18
float[ FIGHTER_VERTS ] fighter_vx = { 0, 1.7, -1.7, -1.7, 1.7, 0,  1.7, 1.7, 8.4, 7.7,  -1.7, -1.7, -8.4, -7.7 };
float[ FIGHTER_VERTS ] fighter_vy = { 0, -1.3, -1.3, 1.3, 1.3, 0,  0, 0, -1.0, -1.0,  0, 0, -1.0, -1.0 };
float[ FIGHTER_VERTS ] fighter_vz = { 5.6, 0, 0, 0, 0, -3.5,  2.1, -2.1, 4.9, 1.4,  2.1, -2.1, 4.9, 1.4 };
int[ FIGHTER_EDGES * 2 ] fighter_edges = {
    0,1, 0,2, 0,3, 0,4,               // nose
    1,2, 2,3, 3,4, 4,1,               // body ring
    5,1, 5,2, 5,3, 5,4,               // tail
    6,8, 8,9, 9,7,                    // right wing
    10,12, 12,13, 13,11               // left wing
};

// trench turret: a squat box with a barrel pointing out of the wall
#define TURRET_VERTS 10
#define TURRET_EDGES 13
float[ TURRET_VERTS ] turret_vx = { 0, 0, 0, 0,  1.6, 1.6, 1.6, 1.6,  1.6, 3.4 };
float[ TURRET_VERTS ] turret_vy = { -1.4, -1.4, 1.4, 1.4,  -1.0, -1.0, 1.0, 1.0,  0, 0 };
float[ TURRET_VERTS ] turret_vz = { -1.4, 1.4, 1.4, -1.4,  -1.0, 1.0, 1.0, -1.0,  0, 0 };
int[ TURRET_EDGES * 2 ] turret_edges = {
    0,1, 1,2, 2,3, 3,0,  4,5, 5,6, 6,7, 7,4,  0,4, 1,5, 2,6, 3,7,  8,9
};

// built per object (towers): a box and a pyramid cap
float[ MAX_MODEL_VERTS ] mv_x;
float[ MAX_MODEL_VERTS ] mv_y;
float[ MAX_MODEL_VERTS ] mv_z;
int[ 32 ] tower_edges = { 0,1, 1,2, 2,3, 3,0,  4,5, 5,6, 6,7, 7,4,  0,4, 1,5, 2,6, 3,7,  4,8, 5,8, 6,8, 7,8 };

float[ MAX_MODEL_VERTS ] tv_x;
float[ MAX_MODEL_VERTS ] tv_y;
float[ MAX_MODEL_VERTS ] tv_z;
float[ MAX_MODEL_VERTS ] sv_x;
float[ MAX_MODEL_VERTS ] sv_y;

float world_intensity = 1.0;

float depth_fade( float z )
{
    if( z <= 60 ) return 1.0;
    if( z >= FAR ) return 0.22;
    return 1.0 - 0.78 * ( z - 60 ) / ( FAR - 60 );
}

// draws a model at camera-relative ( x, y, z ), turned by yaw (about the
// vertical) and roll (about its own length); false when it was culled
bool draw_model( float* vx, float* vy, float* vz, int verts, int* edges, int edge_count,
                 float x, float y, float z, float yaw, float roll, float radius )
{
    float cyw, syw, cr, sr, rx, ry, inv;
    int i, e0, e1, saved_glow;
    bool fast = true;

    if( z < -radius || z > FAR + radius ) return false;
    if( fabs( x ) > z * 0.8 + radius + 2 ) return false;
    if( fabs( y ) > z * 0.46 + radius + 2 ) return false;

    cyw = cos( yaw );  syw = sin( yaw );
    cr = cos( roll );  sr = sin( roll );
    for( i = 0; i < verts; i++ )
    {
        rx = vx[ i ] * cr - vy[ i ] * sr;
        ry = vx[ i ] * sr + vy[ i ] * cr;
        tv_x[ i ] = x + rx * cyw + vz[ i ] * syw;
        tv_z[ i ] = z - rx * syw + vz[ i ] * cyw;
        tv_y[ i ] = y + ry;

        // fast path: every corner in front and on screen -> plain 2D lines
        if( fast )
        {
            if( tv_z[ i ] < 1.0 ) fast = false;
            else
            {
                inv = FOCAL / tv_z[ i ];
                sv_x[ i ] = SCREEN_CX + tv_x[ i ] * inv;
                sv_y[ i ] = SCREEN_CY + view_dy + tv_y[ i ] * inv;
                if( sv_x[ i ] < -30 || sv_x[ i ] > 670 || sv_y[ i ] < -30 || sv_y[ i ] > 390 ) fast = false;
            }
        }
    }

    vec_intensity( world_intensity * depth_fade( z ) );
    saved_glow = vec_current_glow;
    if( z < 30 ) vec_current_glow = 0;        // close up the lines are long: skip the halo

    if( fast )
      for( i = 0; i < edge_count; i++ )
      {
          e0 = edges[ i * 2 ];
          e1 = edges[ i * 2 + 1 ];
          vec_line( sv_x[ e0 ], sv_y[ e0 ], sv_x[ e1 ], sv_y[ e1 ] );
      }
    else
      for( i = 0; i < edge_count; i++ )
      {
          e0 = edges[ i * 2 ];
          e1 = edges[ i * 2 + 1 ];
          vec_line_3d( tv_x[ e0 ], tv_y[ e0 ], tv_z[ e0 ], tv_x[ e1 ], tv_y[ e1 ], tv_z[ e1 ] );
      }

    vec_current_glow = saved_glow;
    vec_intensity( world_intensity );
    return true;
}

// a 3D line in camera-relative coordinates, faded by its nearer end
void line_3d_faded( float x1, float y1, float z1, float x2, float y2, float z2 )
{
    vec_intensity( world_intensity * depth_fade( fmin( z1, z2 ) ) );
    vec_line_3d( x1, y1, z1, x2, y2, z2 );
}

// -----------------------------------------------------------------------------
//   DEBRIS (explosions; visual only). World coordinates: they scroll past
// -----------------------------------------------------------------------------

#define MAX_DEBRIS 48
#define DEBRIS_CYCLE_BUDGET 150000

float[ MAX_DEBRIS ] db_x;
float[ MAX_DEBRIS ] db_y;
float[ MAX_DEBRIS ] db_z;
float[ MAX_DEBRIS ] db_vx;
float[ MAX_DEBRIS ] db_vy;
float[ MAX_DEBRIS ] db_vz;
float[ MAX_DEBRIS ] db_len;
float[ MAX_DEBRIS ] db_ang;
float[ MAX_DEBRIS ] db_spin;
int[ MAX_DEBRIS ] db_life;
int[ MAX_DEBRIS ] db_color;
int next_debris = 0;

void blow_up( float x, float y, float z, int count, float power, float length, int color )
{
    int k, i;
    float a, b;
    for( k = 0; k < count; k++ )
    {
        i = next_debris;
        next_debris = ( next_debris + 1 ) % MAX_DEBRIS;
        a = fx_range( 0, 6.2831853 );
        b = fx_range( -1.2, 1.2 );
        db_x[ i ] = x;  db_y[ i ] = y;  db_z[ i ] = z;
        db_vx[ i ] = cos( a ) * cos( b ) * power * fx_range( 0.4, 1.0 );
        db_vy[ i ] = sin( b ) * power * fx_range( 0.4, 1.0 );
        db_vz[ i ] = sin( a ) * cos( b ) * power * fx_range( 0.4, 1.0 );
        db_len[ i ] = length * fx_range( 0.5, 1.3 );
        db_ang[ i ] = fx_range( 0, 6.28 );
        db_spin[ i ] = fx_range( -0.3, 0.3 );
        db_life[ i ] = 50 + fx_range( 0, 40 );
        db_color[ i ] = color;
    }
}

void update_debris()
{
    int i;
    for( i = 0; i < MAX_DEBRIS; i++ )
      if( db_life[ i ] > 0 )
      {
          db_life[ i ]--;
          db_x[ i ] += db_vx[ i ] - turn_x * db_z[ i ];
          db_y[ i ] += db_vy[ i ] - turn_y * db_z[ i ];
          db_z[ i ] += db_vz[ i ] - scroll;
          db_ang[ i ] += db_spin[ i ];
          if( db_z[ i ] < 1 ) db_life[ i ] = 0;
      }
}

void draw_debris()
{
    int i;
    float x, y, z, hx, hy, f;
    vec_thickness( 1.8 );
    vec_glow_size( 10 );
    for( i = 0; i < MAX_DEBRIS; i++ )
      if( db_life[ i ] > 0 )
      {
          if( get_cycle_counter() > DEBRIS_CYCLE_BUDGET ) break;   // a busy frame: skip the rest
          x = db_x[ i ] - ship_x;  y = db_y[ i ] - ship_y;  z = db_z[ i ];
          if( z < 4 || z > FAR ) continue;
          f = fmin( 1.0, db_life[ i ] / 25.0 );
          vec_color( db_color[ i ], 0 );
          if( z > 30 ) vec_color( db_color[ i ], vec_scale_rgb( db_color[ i ], 0.3 ) );
          vec_intensity( world_intensity * f * depth_fade( z ) );
          hx = cos( db_ang[ i ] ) * db_len[ i ] * 0.5;
          hy = sin( db_ang[ i ] ) * db_len[ i ] * 0.5;
          vec_line_3d( x - hx, y - hy, z, x + hx, y + hy, z + hx * 0.4 );
      }
    vec_intensity( world_intensity );
}

// -----------------------------------------------------------------------------
//   STARS (space phase and title)
// -----------------------------------------------------------------------------

#define STARS 70
float[ STARS ] st_x;
float[ STARS ] st_y;
float[ STARS ] st_z;

void init_stars()
{
    int i;
    for( i = 0; i < STARS; i++ )
    {
        st_x[ i ] = fx_range( -320, 320 );
        st_y[ i ] = fx_range( -200, 200 );
        st_z[ i ] = fx_range( 20, FAR );
    }
}

void update_stars( float speed )
{
    int i;
    for( i = 0; i < STARS; i++ )
    {
        st_z[ i ] -= speed;
        st_x[ i ] -= turn_x * st_z[ i ];
        st_y[ i ] -= turn_y * st_z[ i ];
        if( st_z[ i ] < 8 || fabs( st_x[ i ] ) > st_z[ i ] * 1.2 + 40 || fabs( st_y[ i ] ) > st_z[ i ] * 0.8 + 40 )
        {
            st_x[ i ] = fx_range( -320, 320 );
            st_y[ i ] = fx_range( -200, 200 );
            st_z[ i ] = FAR;
        }
    }
}

void draw_stars( float streak )
{
    int i;
    float x1, y1, x2, y2;
    vec_thickness( 1.6 );
    vec_glow_size( 0 );
    vec_color( 0xFFFFE8E0, 0 );
    for( i = 0; i < STARS; i++ )
    {
        if( !vec_project( st_x[ i ], st_y[ i ], st_z[ i ], &x1, &y1 ) ) continue;
        if( !vec_project( st_x[ i ], st_y[ i ], st_z[ i ] + streak, &x2, &y2 ) ) continue;
        if( x1 < 0 || x1 > 640 || y1 < 0 || y1 > 360 ) continue;
        vec_intensity( world_intensity * ( 1.15 - st_z[ i ] / FAR ) );
        vec_line( x1, y1, x2 + 0.5, y2 );
    }
    vec_intensity( world_intensity );
}

// -----------------------------------------------------------------------------
//   FIREBALLS: the enemy's shots, which you can shoot down
// -----------------------------------------------------------------------------

#define MAX_FB 10

bool[ MAX_FB ] fb_active;
float[ MAX_FB ] fb_x;
float[ MAX_FB ] fb_y;
float[ MAX_FB ] fb_z;
float[ MAX_FB ] fb_vx;
float[ MAX_FB ] fb_vy;
float[ MAX_FB ] fb_vz;
float[ MAX_FB ] fb_spin;

void shield_hit();

float fireball_speed()
{
    return fmin( 1.6 + 0.18 * ( wave - 1 ), 3.4 ) * diff_pickf( 0.85, 1.0, 1.15 );
}

// launches a fireball from ( x, y, z ) at the ship, offset sideways by ox
void launch_fireball_at( float x, float y, float z, float ox )
{
    int k;
    float dx, dy, dz, d, sp;
    for( k = 0; k < MAX_FB; k++ )
      if( !fb_active[ k ] )
      {
          fb_active[ k ] = true;
          fb_x[ k ] = x;  fb_y[ k ] = y;  fb_z[ k ] = z;
          // aim where the ship will be, roughly; the world's own approach
          // (scroll) carries it the rest of the way
          dx = ship_x + ox + frand_range( -1.5, 1.5 ) - x;
          dy = ship_y + frand_range( -1.5, 1.5 ) - y;
          dz = -z;
          d = sqrt( dx * dx + dy * dy + dz * dz ) + 0.001;
          sp = fireball_speed();
          fb_vx[ k ] = dx / d * sp;
          fb_vy[ k ] = dy / d * sp;
          fb_vz[ k ] = dz / d * sp;
          fb_spin[ k ] = 0;
          sfx( SND_FIREBALL, 0.32 );
          return;
      }
}

void launch_fireball( float x, float y, float z )
{
    launch_fireball_at( x, y, z, 0 );
}

void update_fireballs()
{
    int k;
    for( k = 0; k < MAX_FB; k++ )
      if( fb_active[ k ] )
      {
          fb_x[ k ] += fb_vx[ k ] - turn_x * fb_z[ k ];
          fb_y[ k ] += fb_vy[ k ] - turn_y * fb_z[ k ];
          fb_z[ k ] += fb_vz[ k ] - scroll;
          fb_spin[ k ] += 0.25;
          if( fb_z[ k ] < 1.5 )
          {
              fb_active[ k ] = false;
              if( fabs( fb_x[ k ] - ship_x ) < 5 && fabs( fb_y[ k ] - ship_y ) < 5 ) shield_hit();
          }
      }
}

void draw_fireballs()
{
    int k, j;
    float sx, sy, r, a;
    vec_thickness( 2.2 );
    vec_glow_size( 14 );
    vec_color( 0xFF40B0FF, 0xFF103070 );
    for( k = 0; k < MAX_FB; k++ )
      if( fb_active[ k ] && fb_z[ k ] > 1 )
        if( vec_project( fb_x[ k ] - ship_x, fb_y[ k ] - ship_y, fb_z[ k ], &sx, &sy ) )
        {
            r = fmin( 60, fmax( 3, 1.7 * FOCAL / fb_z[ k ] ) );
            vec_intensity( world_intensity * fmax( 0.45, depth_fade( fb_z[ k ] ) ) );
            // a spinning four-point star round a glowing core
            for( j = 0; j < 4; j++ )
            {
                a = fb_spin[ k ] + j * 1.5707963;
                vec_line( sx + cos( a ) * r * 0.25, sy + sin( a ) * r * 0.25, sx + cos( a ) * r, sy + sin( a ) * r );
            }
            vec_circle( sx, sy, r * 0.35, 6 );
        }
    vec_intensity( world_intensity );
}

// -----------------------------------------------------------------------------
//   PHASE 1: INTERCEPTORS
// -----------------------------------------------------------------------------

#define MAX_FIGHTERS 4
#define FM_APPROACH  0
#define FM_STRAFE    1
#define FM_RETREAT   2

bool[ MAX_FIGHTERS ] ft_active;
float[ MAX_FIGHTERS ] ft_x;
float[ MAX_FIGHTERS ] ft_y;
float[ MAX_FIGHTERS ] ft_z;
float[ MAX_FIGHTERS ] ft_vx;
float[ MAX_FIGHTERS ] ft_vy;
float[ MAX_FIGHTERS ] ft_vz;
float[ MAX_FIGHTERS ] ft_tx;
float[ MAX_FIGHTERS ] ft_ty;
float[ MAX_FIGHTERS ] ft_tz;
int[ MAX_FIGHTERS ] ft_mode;
int[ MAX_FIGHTERS ] ft_reload;

int kills = 0;                    // interceptors downed this wave
int kill_quota = 6;
int spawn_timer = 0;
float dn_x = 320, dn_y = 150;     // the distant dreadnought, on screen

int fighters_wanted()
{
    return min( 2 + wave / 2, MAX_FIGHTERS );
}

float fighter_speed()
{
    return fmin( 1.7 + 0.15 * ( wave - 1 ), 3.2 );
}

void fighter_target( int i )
{
    if( ft_mode[ i ] == FM_APPROACH )
    {
        ft_tx[ i ] = frand_range( -40, 40 );
        ft_ty[ i ] = frand_range( -24, 24 );
        ft_tz[ i ] = frand_range( 55, 95 );
    }
    else if( ft_mode[ i ] == FM_STRAFE )
    {
        // sweep across in front of you
        ft_tx[ i ] = frand_range( 55, 90 );
        if( ft_x[ i ] > 0 ) ft_tx[ i ] = -ft_tx[ i ];
        ft_ty[ i ] = frand_range( -30, 30 );
        ft_tz[ i ] = frand_range( 35, 60 );
    }
    else
    {
        ft_tx[ i ] = frand_range( -160, 160 );
        ft_ty[ i ] = frand_range( -100, 70 );
        ft_tz[ i ] = frand_range( 220, 320 );
    }
}

void spawn_fighter()
{
    int i;
    for( i = 0; i < MAX_FIGHTERS; i++ )
      if( !ft_active[ i ] ) break;
    if( i >= MAX_FIGHTERS ) return;

    ft_active[ i ] = true;
    ft_x[ i ] = frand_range( -140, 140 );
    ft_y[ i ] = frand_range( -70, 50 );
    ft_z[ i ] = FAR - 20;
    ft_vx[ i ] = 0;  ft_vy[ i ] = 0;  ft_vz[ i ] = -fighter_speed();
    ft_mode[ i ] = FM_APPROACH;
    ft_reload[ i ] = 60 + grand() % 60;
    fighter_target( i );
}

int count_fighters()
{
    int i, n = 0;
    for( i = 0; i < MAX_FIGHTERS; i++ )
      if( ft_active[ i ] ) n++;
    return n;
}

void update_fighters()
{
    int i;
    float dx, dy, dz, d, sp = fighter_speed();

    if( kills + count_fighters() < kill_quota + 1 && count_fighters() < fighters_wanted() )
    {
        spawn_timer--;
        if( spawn_timer <= 0 )
        {
            spawn_fighter();
            spawn_timer = 70 + grand() % 50;
        }
    }

    for( i = 0; i < MAX_FIGHTERS; i++ )
    {
        if( !ft_active[ i ] ) continue;

        // steer toward the current waypoint
        dx = ft_tx[ i ] - ft_x[ i ];
        dy = ft_ty[ i ] - ft_y[ i ];
        dz = ft_tz[ i ] - ft_z[ i ];
        d = sqrt( dx * dx + dy * dy + dz * dz ) + 0.001;
        ft_vx[ i ] += ( dx / d * sp - ft_vx[ i ] ) * 0.045;
        ft_vy[ i ] += ( dy / d * sp - ft_vy[ i ] ) * 0.045;
        ft_vz[ i ] += ( dz / d * sp - ft_vz[ i ] ) * 0.045;
        ft_x[ i ] += ft_vx[ i ] - turn_x * ft_z[ i ];
        ft_y[ i ] += ft_vy[ i ] - turn_y * ft_z[ i ];
        ft_z[ i ] += ft_vz[ i ];

        if( d < 10 )
        {
            ft_mode[ i ] = ( ft_mode[ i ] + 1 ) % 3;
            fighter_target( i );
        }

        // flew past you: it comes round again from far off
        if( ft_z[ i ] < 4 )
        {
            ft_z[ i ] = FAR - 20;
            ft_mode[ i ] = FM_APPROACH;
            fighter_target( i );
        }

        // fire when in range
        ft_reload[ i ]--;
        if( ft_reload[ i ] <= 0 && ft_z[ i ] > 30 && ft_z[ i ] < 260 && fabs( ft_x[ i ] ) < ft_z[ i ] * 0.7 )
        {
            launch_fireball( ft_x[ i ], ft_y[ i ], ft_z[ i ] - 3 );
            ft_reload[ i ] = ( max( 60, 190 - 14 * wave ) + grand() % 80 ) * diff_pickf( 1.3, 1.0, 0.75 );
        }
    }
}

// the dreadnought's outline for its class, centred at ( x, y ); the caller sets the colour
void draw_dn_shape( float x, float y, float s, float inten )
{
    float w = 150 * s, h = 22 * s;
    vec_thickness( 1.5 );
    vec_glow_size( 8 );
    vec_intensity( world_intensity * inten );
    if( dn_class() == CLASS_CARRIER )
    {
        // a broad, blunt carrier with launch bays along its flank
        h = h * 1.8;  w = w * 0.85;
        vec_line( x - w, y - h * 0.6, x + w, y - h * 0.8 );
        vec_line( x + w, y - h * 0.8, x + w * 1.1, y );
        vec_line( x + w * 1.1, y, x + w, y + h * 0.8 );
        vec_line( x + w, y + h * 0.8, x - w, y + h * 0.6 );
        vec_line( x - w, y + h * 0.6, x - w, y - h * 0.6 );
        vec_line( x - w * 0.8, y - h * 0.05, x + w, y - h * 0.15 );
        vec_line( x - w * 0.5, y + h * 0.25, x - w * 0.2, y + h * 0.25 );
        vec_line( x, y + h * 0.25, x + w * 0.3, y + h * 0.25 );
        vec_line( x + w * 0.5, y + h * 0.25, x + w * 0.8, y + h * 0.25 );
        vec_intensity( world_intensity );
        return;
    }
    if( dn_class() == CLASS_BASTION )
    {
        // a stepped fortress bristling with spires
        vec_line( x - w, y + h, x - w, y - h * 0.5 );
        vec_line( x - w, y - h * 0.5, x - w * 0.4, y - h * 0.5 );
        vec_line( x - w * 0.4, y - h * 0.5, x - w * 0.4, y - h * 1.3 );
        vec_line( x - w * 0.4, y - h * 1.3, x + w * 0.4, y - h * 1.3 );
        vec_line( x + w * 0.4, y - h * 1.3, x + w * 0.4, y - h * 0.5 );
        vec_line( x + w * 0.4, y - h * 0.5, x + w, y - h * 0.5 );
        vec_line( x + w, y - h * 0.5, x + w, y + h );
        vec_line( x + w, y + h, x - w, y + h );
        vec_line( x - w * 0.9, y + h * 0.2, x + w * 0.9, y + h * 0.2 );
        vec_line( x - w * 0.2, y - h * 1.3, x - w * 0.2, y - h * 2.2 );
        vec_line( x + w * 0.2, y - h * 1.3, x + w * 0.2, y - h * 2.0 );
        vec_line( x - w * 0.75, y - h * 0.5, x - w * 0.75, y - h * 1.1 );
        vec_line( x + w * 0.75, y - h * 0.5, x + w * 0.75, y - h * 1.1 );
        vec_intensity( world_intensity );
        return;
    }
    // a long faceted hull with a trench down its spine and gun towers
    vec_line( x - w, y, x - w * 0.7, y - h );
    vec_line( x - w * 0.7, y - h, x + w * 0.85, y - h * 0.4 );
    vec_line( x + w * 0.85, y - h * 0.4, x + w, y );
    vec_line( x + w, y, x + w * 0.85, y + h * 0.5 );
    vec_line( x + w * 0.85, y + h * 0.5, x - w * 0.7, y + h * 1.2 );
    vec_line( x - w * 0.7, y + h * 1.2, x - w, y );
    vec_line( x - w * 0.65, y - h * 0.15, x + w * 0.85, y - h * 0.05 );
    vec_line( x - w * 0.3, y - h * 0.82, x - w * 0.3, y - h * 1.5 );
    vec_line( x + w * 0.2, y - h * 0.65, x + w * 0.2, y - h * 1.3 );
    vec_line( x + w * 0.5, y - h * 0.55, x + w * 0.5, y - h * 1.0 );
    vec_intensity( world_intensity );
}

void draw_dreadnought_far()
{
    vec_color( 0xFF808080, 0 );
    draw_dn_shape( dn_x, dn_y, 0.25 + 0.75 * kills / kill_quota, 0.7 );
}

void draw_fighters()
{
    int i;
    vec_thickness( 2 );
    vec_glow_size( 14 );
    vec_color( 0xFF80FF80, 0xFF207020 );
    for( i = 0; i < MAX_FIGHTERS; i++ )
      if( ft_active[ i ] )
        draw_model( fighter_vx, fighter_vy, fighter_vz, FIGHTER_VERTS, fighter_edges, FIGHTER_EDGES,
                    ft_x[ i ], ft_y[ i ], ft_z[ i ], safe_atan2( ft_vx[ i ], ft_vz[ i ] ),
                    clampf( -ft_vx[ i ] * 0.5, -0.9, 0.9 ), 10 );
}

// -----------------------------------------------------------------------------
//   BOMBERS (from wave 2): slow, tough, and they fire spreads
// -----------------------------------------------------------------------------

#define MAX_BOMBERS 2
#define BOMBER_VERTS 9
#define BOMBER_EDGES 14
float[ BOMBER_VERTS ] bomber_vx = { 0, 11, 4.5, 0, -4.5, -11, 0, 3, -3 };
// pitched nose-down so you see the wing from above as it comes across
float[ BOMBER_VERTS ] bomber_vy = { 2.25, -1.075, -0.9, -0.225, -0.9, -1.075, -1.975, -1.725, -1.725 };
float[ BOMBER_VERTS ] bomber_vz = { 5, -3.5, -2, -0.5, -2, -3.5, 0.5, -2.5, -2.5 };
int[ BOMBER_EDGES * 2 ] bomber_edges = {
    0,1, 1,2, 2,3, 3,4, 4,5, 5,0,     // the wing's outline, with its notched tail
    0,6, 1,6, 5,6, 6,7, 6,8, 7,2, 8,4, 7,8
};

bool[ MAX_BOMBERS ] bb_active;
float[ MAX_BOMBERS ] bb_x;
float[ MAX_BOMBERS ] bb_y;
float[ MAX_BOMBERS ] bb_z;
float[ MAX_BOMBERS ] bb_vx;
int[ MAX_BOMBERS ] bb_hp;
int[ MAX_BOMBERS ] bb_reload;
int[ MAX_BOMBERS ] bb_hit;        // frames of white flash after a hit
int bomber_timer = 0;

int bombers_wanted()
{
    int n = 1;
    if( wave >= 4 ) n++;
    if( dn_class() == CLASS_CARRIER ) n++;
    return min( n, MAX_BOMBERS );
}

int count_bombers()
{
    int i, n = 0;
    for( i = 0; i < MAX_BOMBERS; i++ )
      if( bb_active[ i ] ) n++;
    return n;
}

void update_bombers()
{
    int i;
    if( wave < 2 ) return;

    bomber_timer--;
    if( bomber_timer <= 0 && count_bombers() < bombers_wanted() )
      for( i = 0; i < MAX_BOMBERS; i++ )
        if( !bb_active[ i ] )
        {
            bb_active[ i ] = true;
            bb_x[ i ] = -190;
            bb_vx[ i ] = 0.7 + 0.05 * wave;
            if( grand() % 2 ) { bb_x[ i ] = 190; bb_vx[ i ] = -bb_vx[ i ]; }
            bb_y[ i ] = frand_range( -50, 20 );
            bb_z[ i ] = frand_range( 140, 220 );
            bb_hp[ i ] = 3;
            bb_reload[ i ] = 90;
            bb_hit[ i ] = 0;
            bomber_timer = 420 + grand() % 240;
            if( dn_class() == CLASS_CARRIER ) bomber_timer = 260 + grand() % 160;
            break;
        }

    for( i = 0; i < MAX_BOMBERS; i++ )
    {
        if( !bb_active[ i ] ) continue;
        bb_x[ i ] += bb_vx[ i ] - turn_x * bb_z[ i ];
        bb_y[ i ] += 0.15 * sin( anim * 0.02 + i ) - turn_y * bb_z[ i ];
        if( bb_hit[ i ] > 0 ) bb_hit[ i ]--;
        if( fabs( bb_x[ i ] ) > 220 ) { bb_active[ i ] = false; continue; }

        // a spread of three: the middle one comes straight at you
        bb_reload[ i ]--;
        if( bb_reload[ i ] <= 0 && fabs( bb_x[ i ] ) < bb_z[ i ] * 0.6 )
        {
            launch_fireball_at( bb_x[ i ] - 4, bb_y[ i ], bb_z[ i ] - 4, -8 );
            launch_fireball_at( bb_x[ i ], bb_y[ i ], bb_z[ i ] - 4, 0 );
            launch_fireball_at( bb_x[ i ] + 4, bb_y[ i ], bb_z[ i ] - 4, 8 );
            bb_reload[ i ] = ( 170 + grand() % 60 ) * diff_pickf( 1.3, 1.0, 0.75 );
        }
    }
}

void draw_bombers()
{
    int i;
    vec_thickness( 2.2 );
    vec_glow_size( 14 );
    for( i = 0; i < MAX_BOMBERS; i++ )
      if( bb_active[ i ] )
      {
          vec_color( 0xFFFF70FF, 0xFF702070 );
          if( bb_hit[ i ] > 0 ) vec_color( 0xFFFFFFFF, 0xFF707070 );
          draw_model( bomber_vx, bomber_vy, bomber_vz, BOMBER_VERTS, bomber_edges, BOMBER_EDGES,
                      bb_x[ i ], bb_y[ i ], bb_z[ i ], safe_atan2( bb_vx[ i ] * 0.5, -1.0 ), 0, 12 );
      }
}

// -----------------------------------------------------------------------------
//   HOMING MINES (from wave 3): they come for you; shoot them first
// -----------------------------------------------------------------------------

#define MAX_MINES 3

bool[ MAX_MINES ] mn_active;
float[ MAX_MINES ] mn_x;
float[ MAX_MINES ] mn_y;
float[ MAX_MINES ] mn_z;
float[ MAX_MINES ] mn_vx;
float[ MAX_MINES ] mn_vy;
float[ MAX_MINES ] mn_vz;
int mine_timer = 0;

void update_mines()
{
    int i;
    float d, sp;
    if( wave < 3 ) return;

    mine_timer--;
    if( mine_timer <= 0 )
      for( i = 0; i < MAX_MINES; i++ )
        if( !mn_active[ i ] )
        {
            mn_active[ i ] = true;
            mn_x[ i ] = frand_range( -110, 110 );
            mn_y[ i ] = frand_range( -70, 70 );
            mn_z[ i ] = FAR - 40;
            mn_vx[ i ] = 0;  mn_vy[ i ] = 0;  mn_vz[ i ] = -1;
            mine_timer = max( 160, 360 - 25 * wave ) + grand() % 120;
            break;
        }

    sp = fmin( 1.3 + 0.1 * wave, 2.4 );
    for( i = 0; i < MAX_MINES; i++ )
    {
        if( !mn_active[ i ] ) continue;
        // steer for you
        d = sqrt( mn_x[ i ] * mn_x[ i ] + mn_y[ i ] * mn_y[ i ] + mn_z[ i ] * mn_z[ i ] ) + 0.001;
        mn_vx[ i ] += ( -mn_x[ i ] / d * sp - mn_vx[ i ] ) * 0.03;
        mn_vy[ i ] += ( -mn_y[ i ] / d * sp - mn_vy[ i ] ) * 0.03;
        mn_vz[ i ] += ( -mn_z[ i ] / d * sp - mn_vz[ i ] ) * 0.03;
        mn_x[ i ] += mn_vx[ i ] - turn_x * mn_z[ i ];
        mn_y[ i ] += mn_vy[ i ] - turn_y * mn_z[ i ];
        mn_z[ i ] += mn_vz[ i ];
        if( mn_z[ i ] < 3 )
        {
            mn_active[ i ] = false;
            if( fabs( mn_x[ i ] ) < 7 && fabs( mn_y[ i ] ) < 7 )
            {
                shield_hit();
                blow_up( mn_x[ i ], mn_y[ i ], 8, 10, 0.4, 2.0, 0xFF4040FF );
            }
        }
    }
}

void draw_mines()
{
    int i, j;
    float sx, sy, r, a;
    vec_thickness( 2 );
    vec_glow_size( 12 );
    vec_color( 0xFF4848FF, 0xFF101070 );
    for( i = 0; i < MAX_MINES; i++ )
      if( mn_active[ i ] && mn_z[ i ] > 2 )
        if( vec_project( mn_x[ i ], mn_y[ i ], mn_z[ i ], &sx, &sy ) )
        {
            r = fmin( 50, fmax( 3, 2.4 * FOCAL / mn_z[ i ] ) );
            vec_intensity( world_intensity * fmax( 0.5, depth_fade( mn_z[ i ] ) ) );
            vec_circle( sx, sy, r * 0.5, 8 );
            for( j = 0; j < 6; j++ )
            {
                a = anim * 0.05 + j * 1.0471976;
                vec_line( sx + cos( a ) * r * 0.5, sy + sin( a ) * r * 0.5, sx + cos( a ) * r, sy + sin( a ) * r );
            }
        }
    vec_intensity( world_intensity );
}

// -----------------------------------------------------------------------------
//   PHASE 2: THE HULL AND ITS GUN TOWERS
// -----------------------------------------------------------------------------

#define HULL_Y      14.0          // the hull's surface, below you
#define MAX_TOWERS  10

bool[ MAX_TOWERS ] tw_active;
float[ MAX_TOWERS ] tw_x;
float[ MAX_TOWERS ] tw_z;
float[ MAX_TOWERS ] tw_h;
int[ MAX_TOWERS ] tw_reload;

int towers_spawned = 0;
int towers_quota = 10;
int towers_killed = 0;
int tower_timer = 0;
float hull_travel = 0;

// low plates on the hull: scenery that sweeps past
#define MAX_PLATES 8
bool[ MAX_PLATES ] pl_active;
float[ MAX_PLATES ] pl_x;
float[ MAX_PLATES ] pl_z;
float[ MAX_PLATES ] pl_w;
float[ MAX_PLATES ] pl_d;
int plate_timer = 0;

void update_plates()
{
    int i;
    plate_timer--;
    if( plate_timer <= 0 )
      for( i = 0; i < MAX_PLATES; i++ )
        if( !pl_active[ i ] )
        {
            pl_active[ i ] = true;
            pl_x[ i ] = ship_x + fx_range( -110, 110 );
            pl_z[ i ] = FAR;
            pl_w[ i ] = fx_range( 5, 14 );
            pl_d[ i ] = fx_range( 6, 22 );
            plate_timer = 14 + fx_range( 0, 14 );
            break;
        }
    for( i = 0; i < MAX_PLATES; i++ )
      if( pl_active[ i ] )
      {
          pl_z[ i ] -= scroll;
          if( pl_z[ i ] + pl_d[ i ] < 2 ) pl_active[ i ] = false;
      }
}

// shield generators (from wave 2): each one destroyed knocks out two trench turrets
#define MAX_GENS 4
bool[ MAX_GENS ] gn_active;
float[ MAX_GENS ] gn_x;
float[ MAX_GENS ] gn_z;
int gens_spawned = 0;
int gens_quota = 0;
int gens_destroyed = 0;
int gen_timer = 0;

void update_gens()
{
    int i;
    if( gens_spawned < gens_quota )
    {
        gen_timer--;
        if( gen_timer <= 0 )
          for( i = 0; i < MAX_GENS; i++ )
            if( !gn_active[ i ] )
            {
                gn_active[ i ] = true;
                gn_x[ i ] = ship_x + frand_range( -55, 55 );
                gn_z[ i ] = FAR;
                gens_spawned++;
                gen_timer = 200 + grand() % 120;
                break;
            }
    }
    for( i = 0; i < MAX_GENS; i++ )
      if( gn_active[ i ] )
      {
          gn_z[ i ] -= scroll;
          if( gn_z[ i ] < -10 ) gn_active[ i ] = false;
      }
}

int count_gens()
{
    int i, n = 0;
    for( i = 0; i < MAX_GENS; i++ )
      if( gn_active[ i ] ) n++;
    return n;
}

// a wireframe dome with a pulsing core
void draw_gen( int i )
{
    int k;
    float x = gn_x[ i ] - ship_x, y = HULL_Y - ship_y, z = gn_z[ i ], a0, a1, r = 7, h = 7, p;
    if( z < 3 || z > FAR ) return;
    vec_intensity( world_intensity * depth_fade( z ) );
    for( k = 0; k < 8; k++ )
    {
        a0 = k * 0.785398;  a1 = a0 + 0.785398;
        vec_line_3d( x + cos( a0 ) * r, y, z + sin( a0 ) * r, x + cos( a1 ) * r, y, z + sin( a1 ) * r );
    }
    for( k = 0; k < 4; k++ )
    {
        a0 = k * 0.785398;  a1 = a0 + 0.785398;
        vec_line_3d( x + cos( a0 ) * r, y - sin( a0 ) * h, z, x + cos( a1 ) * r, y - sin( a1 ) * h, z );
        vec_line_3d( x, y - sin( a0 ) * h, z + cos( a0 ) * r, x, y - sin( a1 ) * h, z + cos( a1 ) * r );
    }
    p = 1.2 + 0.6 * sin( anim * 0.2 );
    vec_line_3d( x - p, y - 3, z, x + p, y - 3, z );
    vec_line_3d( x, y - 3 - p, z, x, y - 3 + p, z );
    vec_intensity( world_intensity );
}

float hull_speed()
{
    return fmin( 2.6 + 0.2 * ( wave - 1 ), 4.2 );
}

void update_towers()
{
    int i;

    // new towers rise over the horizon until the quota is reached
    if( towers_spawned < towers_quota )
    {
        tower_timer--;
        if( tower_timer <= 0 )
          for( i = 0; i < MAX_TOWERS; i++ )
            if( !tw_active[ i ] )
            {
                tw_active[ i ] = true;
                tw_x[ i ] = ship_x + frand_range( -70, 70 );
                tw_z[ i ] = FAR;
                tw_h[ i ] = frand_range( 8, 15 );
                tw_reload[ i ] = 30 + grand() % 60;
                towers_spawned++;
                tower_timer = max( 28, 70 - 4 * wave ) + grand() % 30;
                break;
            }
    }

    for( i = 0; i < MAX_TOWERS; i++ )
    {
        if( !tw_active[ i ] ) continue;
        tw_z[ i ] -= scroll;
        if( tw_z[ i ] < -8 ) { tw_active[ i ] = false; continue; }
        tw_reload[ i ]--;
        if( tw_reload[ i ] <= 0 && tw_z[ i ] > 60 && tw_z[ i ] < 260 )
        {
            launch_fireball( tw_x[ i ], HULL_Y - tw_h[ i ] - 3, tw_z[ i ] );
            tw_reload[ i ] = ( max( 60, 170 - 12 * wave ) + grand() % 80 ) * diff_pickf( 1.3, 1.0, 0.75 );
        }
    }
}

int count_towers()
{
    int i, n = 0;
    for( i = 0; i < MAX_TOWERS; i++ )
      if( tw_active[ i ] ) n++;
    return n;
}

void draw_tower( int i )
{
    float s = 4.0, h = -tw_h[ i ];
    mv_x[ 0 ] = -s; mv_z[ 0 ] = -s; mv_y[ 0 ] = 0;
    mv_x[ 1 ] =  s; mv_z[ 1 ] = -s; mv_y[ 1 ] = 0;
    mv_x[ 2 ] =  s; mv_z[ 2 ] =  s; mv_y[ 2 ] = 0;
    mv_x[ 3 ] = -s; mv_z[ 3 ] =  s; mv_y[ 3 ] = 0;
    mv_x[ 4 ] = -s; mv_z[ 4 ] = -s; mv_y[ 4 ] = h;
    mv_x[ 5 ] =  s; mv_z[ 5 ] = -s; mv_y[ 5 ] = h;
    mv_x[ 6 ] =  s; mv_z[ 6 ] =  s; mv_y[ 6 ] = h;
    mv_x[ 7 ] = -s; mv_z[ 7 ] =  s; mv_y[ 7 ] = h;
    mv_x[ 8 ] = 0;  mv_z[ 8 ] = 0;  mv_y[ 8 ] = h - 4.5;
    draw_model( mv_x, mv_y, mv_z, 9, tower_edges, 16, tw_x[ i ] - ship_x, HULL_Y - ship_y, tw_z[ i ], 0.785, 0, 12 );
}

void draw_hull()
{
    int k;
    float off, x, z;

    // the hull: lines running ahead, and cross lines sweeping toward you
    vec_thickness( 1.4 );
    vec_glow_size( 0 );
    vec_color( class_dim(), 0 );
    off = ship_x - floor( ship_x / 24 ) * 24;
    for( k = -6; k <= 6; k++ )
    {
        x = k * 24 - off;
        line_3d_faded( x, HULL_Y, 6, x, HULL_Y, FAR );
    }
    off = hull_travel - floor( hull_travel / 30 ) * 30;
    for( k = 1; k <= 13; k++ )
    {
        z = k * 30 - off;
        line_3d_faded( -150, HULL_Y, z, 150, HULL_Y, z );
    }
    vec_intensity( world_intensity );

    // the horizon
    vec_color( class_dim(), 0 );
    vec_intensity( world_intensity * 0.7 );
    z = SCREEN_CY + view_dy + ( HULL_Y - ship_y ) * FOCAL / FAR;
    vec_line( 0, z, 640, z );

    // raised plates, sweeping past
    vec_color( class_core(), 0 );
    for( k = 0; k < MAX_PLATES; k++ )
      if( pl_active[ k ] )
      {
          mv_x[ 0 ] = -pl_w[ k ]; mv_z[ 0 ] = 0;         mv_y[ 0 ] = 0;
          mv_x[ 1 ] =  pl_w[ k ]; mv_z[ 1 ] = 0;         mv_y[ 1 ] = 0;
          mv_x[ 2 ] =  pl_w[ k ]; mv_z[ 2 ] = pl_d[ k ]; mv_y[ 2 ] = 0;
          mv_x[ 3 ] = -pl_w[ k ]; mv_z[ 3 ] = pl_d[ k ]; mv_y[ 3 ] = 0;
          mv_x[ 4 ] = -pl_w[ k ]; mv_z[ 4 ] = 0;         mv_y[ 4 ] = -1.6;
          mv_x[ 5 ] =  pl_w[ k ]; mv_z[ 5 ] = 0;         mv_y[ 5 ] = -1.6;
          mv_x[ 6 ] =  pl_w[ k ]; mv_z[ 6 ] = pl_d[ k ]; mv_y[ 6 ] = -1.6;
          mv_x[ 7 ] = -pl_w[ k ]; mv_z[ 7 ] = pl_d[ k ]; mv_y[ 7 ] = -1.6;
          draw_model( mv_x, mv_y, mv_z, 8, tower_edges, 12, pl_x[ k ] - ship_x, HULL_Y - ship_y, pl_z[ k ], 0, 0, pl_w[ k ] + pl_d[ k ] );
      }
    vec_intensity( world_intensity );

    vec_thickness( 1.8 );
    vec_glow_size( 12 );
    vec_color( 0xFFFFE060, 0xFF605020 );
    for( k = 0; k < MAX_TOWERS; k++ )
      if( tw_active[ k ] ) draw_tower( k );

    vec_thickness( 1.8 );
    vec_glow_size( 10 );
    vec_color( 0xFFFFC0A0, 0xFF604030 );
    for( k = 0; k < MAX_GENS; k++ )
      if( gn_active[ k ] ) draw_gen( k );
}

// -----------------------------------------------------------------------------
//   PHASE 3: THE TRENCH
// -----------------------------------------------------------------------------

#define TRENCH_W     10.0         // half width
#define TRENCH_TOP  -12.0
#define TRENCH_FLOOR 12.0
#define MAX_CATWALKS 8
#define MAX_TURRETS  12

float trench_pos = 0;             // distance travelled down the trench
float trench_len = 2400;
bool vent_hit = false;

bool[ MAX_CATWALKS ] cw_active;
float[ MAX_CATWALKS ] cw_pos;
float[ MAX_CATWALKS ] cw_y;
bool[ MAX_CATWALKS ] cw_warned;

bool[ MAX_TURRETS ] tu_active;
float[ MAX_TURRETS ] tu_pos;
float[ MAX_TURRETS ] tu_side;     // -1 left wall, +1 right wall
float[ MAX_TURRETS ] tu_y;
int[ MAX_TURRETS ] tu_reload;

// laser gates (from wave 3): a beam across the trench that blinks on and off
#define MAX_GATES 4
bool[ MAX_GATES ] lg_active;
float[ MAX_GATES ] lg_pos;
bool[ MAX_GATES ] lg_vertical;    // a vertical beam at x, or a horizontal one at y
float[ MAX_GATES ] lg_at;
int[ MAX_GATES ] lg_offset;
bool[ MAX_GATES ] lg_warned;
int trench_clock = 0;             // drives the gates' blinking (gameplay time)

bool gate_on( int i )
{
    return ( ( trench_clock + lg_offset[ i ] ) % 100 ) < 58;
}

float trench_speed()
{
    return fmin( 3.2 + 0.25 * ( wave - 1 ), 5.0 );
}

// lays out the catwalks and turrets for this run down the trench
void build_trench()
{
    int i, n;
    float gap;

    for( i = 0; i < MAX_CATWALKS; i++ ) cw_active[ i ] = false;
    for( i = 0; i < MAX_TURRETS; i++ ) tu_active[ i ] = false;
    for( i = 0; i < MAX_GATES; i++ ) lg_active[ i ] = false;

    // laser gates from wave 3, more on a Bastion
    n = 0;
    if( wave >= 3 ) n = wave - 2;
    if( wave >= 3 && dn_class() == CLASS_BASTION ) n += 2;
    n = min( n, MAX_GATES );
    if( n > 0 )
    {
        gap = ( trench_len - 900 ) / n;
        for( i = 0; i < n; i++ )
        {
            lg_active[ i ] = true;
            lg_pos[ i ] = 650 + i * gap + frand_range( 0, gap * 0.4 );
            lg_vertical[ i ] = ( grand() % 2 ) == 0;
            if( lg_vertical[ i ] ) lg_at[ i ] = frand_range( -6, 6 );
            else lg_at[ i ] = frand_range( -7, 7 );
            lg_offset[ i ] = grand() % 100;
            lg_warned[ i ] = false;
        }
    }

    // catwalks from wave 2
    n = min( 2 * ( wave - 1 ), MAX_CATWALKS );
    if( n > 0 )
    {
        gap = ( trench_len - 900 ) / n;
        for( i = 0; i < n; i++ )
        {
            cw_active[ i ] = true;
            cw_pos[ i ] = 500 + i * gap + frand_range( 0, gap * 0.5 );
            cw_y[ i ] = frand_range( -8, 8 );
            cw_warned[ i ] = false;
        }
    }

    // each shield generator destroyed on the hull knocks out two turrets
    n = max( 2, min( 4 + 2 * wave, MAX_TURRETS ) - 2 * gens_destroyed );
    gap = ( trench_len - 700 ) / n;
    for( i = 0; i < n; i++ )
    {
        tu_active[ i ] = true;
        tu_pos[ i ] = 350 + i * gap + frand_range( 0, gap * 0.6 );
        tu_side[ i ] = 1;
        if( grand() % 2 ) tu_side[ i ] = -1;
        tu_y[ i ] = frand_range( -8, 9 );
        tu_reload[ i ] = 20 + grand() % 40;
    }
}

void update_trench()
{
    int i;
    float z;

    trench_pos += scroll;
    trench_clock++;

    for( i = 0; i < MAX_GATES; i++ )
    {
        if( !lg_active[ i ] ) continue;
        z = lg_pos[ i ] - trench_pos;
        if( z < 140 && !lg_warned[ i ] )
        {
            lg_warned[ i ] = true;
            sfx( SND_BEEP, 0.4 );
        }
        if( z < 1.5 )
        {
            lg_active[ i ] = false;
            if( gate_on( i ) )
            {
                if( ( lg_vertical[ i ] && fabs( ship_x - lg_at[ i ] ) < 2.4 )
                    || ( !lg_vertical[ i ] && fabs( ship_y - lg_at[ i ] ) < 2.4 ) )
                {
                    shield_hit();
                    blow_up( ship_x, ship_y, 6, 10, 0.4, 2.0, 0xFF4040FF );
                }
            }
        }
    }

    for( i = 0; i < MAX_CATWALKS; i++ )
    {
        if( !cw_active[ i ] ) continue;
        z = cw_pos[ i ] - trench_pos;
        if( z < 130 && !cw_warned[ i ] )
        {
            cw_warned[ i ] = true;
            sfx( SND_BEEP, 0.4 );
        }
        if( z < 1.5 )
        {
            cw_active[ i ] = false;
            if( fabs( ship_y - cw_y[ i ] ) < 2.6 )
            {
                shield_hit();
                blow_up( ship_x, cw_y[ i ], 6, 10, 0.4, 2.0, 0xFF60E0FF );
            }
        }
    }

    for( i = 0; i < MAX_TURRETS; i++ )
    {
        if( !tu_active[ i ] ) continue;
        z = tu_pos[ i ] - trench_pos;
        if( z < -6 ) { tu_active[ i ] = false; continue; }
        tu_reload[ i ]--;
        if( tu_reload[ i ] <= 0 && z > 40 && z < 230 )
        {
            launch_fireball( tu_side[ i ] * ( TRENCH_W - 3 ), tu_y[ i ], z );
            tu_reload[ i ] = ( max( 50, 150 - 10 * wave ) + grand() % 60 ) * diff_pickf( 1.3, 1.0, 0.75 );
        }
    }
}

void draw_trench()
{
    int k;
    float off, z, sx, sy, l, r, t, b, vz, x1, x2;

    l = -TRENCH_W - ship_x;  r = TRENCH_W - ship_x;
    t = TRENCH_TOP - ship_y; b = TRENCH_FLOOR - ship_y;

    vec_thickness( 1.6 );
    vec_glow_size( 0 );
    vec_color( class_core(), 0 );

    // the long edges, and the hull either side of the trench
    line_3d_faded( l, t, 2, l, t, FAR );
    line_3d_faded( r, t, 2, r, t, FAR );
    line_3d_faded( l, b, 2, l, b, FAR );
    line_3d_faded( r, b, 2, r, b, FAR );
    line_3d_faded( l - 22, t, 6, l - 22, t, FAR );
    line_3d_faded( r + 22, t, 6, r + 22, t, FAR );
    line_3d_faded( l - 50, t, 12, l - 50, t, FAR );
    line_3d_faded( r + 50, t, 12, r + 50, t, FAR );

    // ribs sweeping toward you
    off = trench_pos - floor( trench_pos / 30 ) * 30;
    for( k = 1; k <= 13; k++ )
    {
        z = k * 30 - off;
        if( trench_len - trench_pos + 30 < z ) break;       // past the trench's end
        vec_intensity( world_intensity * depth_fade( z ) );
        vec_line_3d( l, t, z, l, b, z );
        vec_line_3d( l, b, z, r, b, z );
        vec_line_3d( r, b, z, r, t, z );
        if( k % 2 == 0 )
        {
            vec_line_3d( l, t, z, l - 22, t, z );
            vec_line_3d( r, t, z, r + 22, t, z );
        }
    }
    vec_intensity( world_intensity );

    // the trench's end: a wall across it
    vz = trench_len - trench_pos + 26;
    if( vz < FAR && vz > 2 )
    {
        vec_color( class_core(), 0 );
        line_3d_faded( l, t, vz, r, t, vz );
        line_3d_faded( l, b - 8, vz, r, b - 8, vz );
    }

    // catwalks: beams across the trench
    vec_thickness( 2 );
    vec_glow_size( 10 );
    vec_color( 0xFFFFE060, 0xFF605020 );
    for( k = 0; k < MAX_CATWALKS; k++ )
      if( cw_active[ k ] )
      {
          z = cw_pos[ k ] - trench_pos;
          if( z < 2 || z > FAR ) continue;
          vec_intensity( world_intensity * depth_fade( z ) );
          sy = cw_y[ k ] - ship_y;
          vec_line_3d( l, sy - 1.2, z, r, sy - 1.2, z );
          vec_line_3d( l, sy + 1.2, z, r, sy + 1.2, z );
          vec_line_3d( l, sy - 1.2, z + 3, r, sy - 1.2, z + 3 );
          vec_line_3d( l, sy - 1.2, z, l, sy + 1.2, z );
          vec_line_3d( r, sy - 1.2, z, r, sy + 1.2, z );
      }
    vec_intensity( world_intensity );

    // laser gates: emitters always, the beam when it's on
    for( k = 0; k < MAX_GATES; k++ )
      if( lg_active[ k ] )
      {
          z = lg_pos[ k ] - trench_pos;
          if( z < 2 || z > FAR ) continue;
          vec_intensity( world_intensity * depth_fade( z ) );
          vec_thickness( 2 );
          vec_glow_size( 0 );
          vec_color( 0xFF8080FF, 0 );
          if( lg_vertical[ k ] )
          {
              sx = lg_at[ k ] - ship_x;
              vec_line_3d( sx - 1.2, t, z, sx + 1.2, t, z );
              vec_line_3d( sx - 1.2, b, z, sx + 1.2, b, z );
          }
          else
          {
              sy = lg_at[ k ] - ship_y;
              vec_line_3d( l, sy - 1.2, z, l, sy + 1.2, z );
              vec_line_3d( r, sy - 1.2, z, r, sy + 1.2, z );
          }
          if( gate_on( k ) )
          {
              vec_thickness( 3 );
              vec_glow_size( 16 );
              vec_color( 0xFF5050FF, 0xFF1818A0 );
              if( z < 30 ) vec_glow_size( 0 );
          }
          else
          {
              vec_thickness( 1 );
              vec_color( 0xFF303070, 0 );
          }
          if( lg_vertical[ k ] ) vec_line_3d( sx, t, z, sx, b, z );
          else vec_line_3d( l, sy, z, r, sy, z );
      }
    vec_intensity( world_intensity );

    // turrets on the walls
    vec_thickness( 2 );
    vec_glow_size( 10 );
    vec_color( 0xFFFFE060, 0xFF605020 );
    for( k = 0; k < MAX_TURRETS; k++ )
      if( tu_active[ k ] )
      {
          z = tu_pos[ k ] - trench_pos;
          // the model's barrel points along +x; flip it for the right wall
          if( tu_side[ k ] < 0 )
            draw_model( turret_vx, turret_vy, turret_vz, TURRET_VERTS, turret_edges, TURRET_EDGES,
                        -TRENCH_W - ship_x, tu_y[ k ] - ship_y, z, 0, 0, 4 );
          else
            draw_model( turret_vx, turret_vy, turret_vz, TURRET_VERTS, turret_edges, TURRET_EDGES,
                        TRENCH_W - ship_x, tu_y[ k ] - ship_y, z, 3.14159, 0, 4 );
      }

    // the reactor vent, set in the floor at the far end
    vz = trench_len - trench_pos;
    if( vz > 2 && vz < FAR )
    {
        vec_thickness( 2.4 );
        vec_glow_size( 16 );
        vec_color( 0xFF4060FF, 0xFF102070 );
        if( vz < 160 && ( anim / 6 ) % 2 == 0 ) vec_color( 0xFFA0C0FF, 0xFF304080 );
        vec_intensity( world_intensity * fmax( 0.5, depth_fade( vz ) ) );
        x1 = -3.5 - ship_x;  x2 = 3.5 - ship_x;
        vec_line_3d( x1, b, vz - 3.5, x2, b, vz - 3.5 );
        vec_line_3d( x2, b, vz - 3.5, x2, b, vz + 3.5 );
        vec_line_3d( x2, b, vz + 3.5, x1, b, vz + 3.5 );
        vec_line_3d( x1, b, vz + 3.5, x1, b, vz - 3.5 );
        vec_line_3d( x1, b, vz - 3.5, x2, b, vz + 3.5 );
        vec_line_3d( x2, b, vz - 3.5, x1, b, vz + 3.5 );
        if( vec_project( -ship_x, b, vz, &sx, &sy ) ) vec_circle( sx, sy, fmax( 3, 1.6 * FOCAL / vz ), 8 );
        vec_intensity( world_intensity );
    }
}

// -----------------------------------------------------------------------------
//   LASERS
// -----------------------------------------------------------------------------

#define TG_NONE     0
#define TG_FIGHTER  1
#define TG_FIREBALL 2
#define TG_TOWER    3
#define TG_TURRET   4
#define TG_VENT     5
#define TG_BOMBER   6
#define TG_MINE     7
#define TG_GEN      8
#define TG_CELL     9

// -----------------------------------------------------------------------------
//   SECRET: THE CORE. Hit the vent with your only shot in the trench and you
//   find a way inside: a tunnel to the reactor lined with energy cells
// -----------------------------------------------------------------------------

#define MAX_CELLS   24
#define CORE_R      14.0
#define CORE_SPEED  2.6
#define CELL_GAP    80
#define CELL_FIRST  260

bool[ MAX_CELLS ] ce_active;
float[ MAX_CELLS ] ce_x;
float[ MAX_CELLS ] ce_y;
float[ MAX_CELLS ] ce_pos;
int cells_hit = 0;
float core_pos = 0;
float core_len = 2300;
bool from_core = false;           // the dreadnought blew up from inside

void start_phase( int p );
void add_score( int points );

void build_core()
{
    int i;
    float a, r;
    for( i = 0; i < MAX_CELLS; i++ )
    {
        a = i * 2.3999632;                        // the golden angle: spread round the tunnel
        r = ( 0.3 + 0.12 * ( ( i * 7 ) % 5 ) ) * CORE_R;
        ce_active[ i ] = true;
        ce_x[ i ] = cos( a ) * r;
        ce_y[ i ] = sin( a ) * r;
        ce_pos[ i ] = CELL_FIRST + i * CELL_GAP;
    }
    cells_hit = 0;
    core_pos = 0;
    core_len = CELL_FIRST + ( MAX_CELLS - 1 ) * CELL_GAP + 200;
}

void update_core()
{
    int i;
    float tx = ( aim_x - SCREEN_CX ) / 300.0 * ( CORE_R - 4 );
    float ty = ( aim_y - SCREEN_CY ) / 160.0 * ( CORE_R - 4 );
    float d;
    scroll = CORE_SPEED;
    ship_x += ( tx - ship_x ) * 0.12;
    ship_y += ( ty - ship_y ) * 0.12;
    d = sqrt( ship_x * ship_x + ship_y * ship_y );
    if( d > CORE_R - 4 )
    {
        ship_x *= ( CORE_R - 4 ) / d;
        ship_y *= ( CORE_R - 4 ) / d;
    }
    core_pos += scroll;
    for( i = 0; i < MAX_CELLS; i++ )
      if( ce_active[ i ] && ce_pos[ i ] - core_pos < 2 ) ce_active[ i ] = false;

    if( core_pos >= core_len )
    {
        from_core = true;
        start_phase( PH_BOOM );
        if( cells_hit == MAX_CELLS )
        {
            add_score( 20000 );
            award_feat( FEAT_CORE );
            banner_sub = "PERFECT CORE - BONUS 20000";
        }
        else banner_sub = "THE REACTOR GOES UP FROM INSIDE";
    }
}

void draw_core()
{
    int k, j;
    float z, off, sx, sy, r, a, spin, pulse, s, x, y;

    // the tunnel: octagonal rings sweeping toward you, slowly turning
    vec_thickness( 1.6 );
    vec_glow_size( 0 );
    vec_color( 0xFFFF60C0, 0 );
    spin = anim * 0.01;
    off = core_pos - floor( core_pos / 26 ) * 26;
    for( k = 1; k <= 15; k++ )
    {
        z = k * 26 - off;
        if( z < 20 ) continue;
        if( core_len - core_pos + 40 < z ) break;         // past the reactor
        if( !vec_project( -ship_x, -ship_y, z, &sx, &sy ) ) continue;
        r = CORE_R * FOCAL / z;
        pulse = 0.75 + 0.25 * sin( anim * 0.15 - z * 0.05 );
        vec_intensity( world_intensity * depth_fade( z ) * pulse );
        for( j = 0; j < 8; j++ )
        {
            a = j * 0.7853982 + spin;
            vec_line( sx + cos( a ) * r, sy + sin( a ) * r, sx + cos( a + 0.7853982 ) * r, sy + sin( a + 0.7853982 ) * r );
        }
    }
    vec_intensity( world_intensity );
    vec_color( 0xFFC040A0, 0 );
    for( j = 0; j < 8; j++ )
    {
        a = j * 0.7853982 + spin;
        line_3d_faded( cos( a ) * CORE_R - ship_x, sin( a ) * CORE_R - ship_y, 4,
                       cos( a ) * CORE_R - ship_x, sin( a ) * CORE_R - ship_y, FAR );
    }

    // the reactor's heart at the far end
    z = core_len - core_pos + 40;
    if( z < FAR && z > 4 && vec_project( -ship_x, -ship_y, z, &sx, &sy ) )
    {
        vec_thickness( 2.4 );
        vec_glow_size( 16 );
        vec_color( 0xFFFFE0FF, 0xFF603070 );
        vec_intensity( world_intensity * depth_fade( z ) );
        for( k = 1; k <= 3; k++ )
          vec_circle( sx, sy, k * 3.5 * FOCAL / z * ( 1 + 0.12 * sin( anim * 0.3 + k ) ), 16 );
        vec_intensity( world_intensity );
    }

    // the energy cells: spinning diamonds
    vec_thickness( 2 );
    for( k = 0; k < MAX_CELLS; k++ )
    {
        if( !ce_active[ k ] ) continue;
        z = ce_pos[ k ] - core_pos;
        if( z < 3 || z > FAR ) continue;
        if( !vec_project( ce_x[ k ] - ship_x, ce_y[ k ] - ship_y, z, &x, &y ) ) continue;
        s = 2.8 * FOCAL / z;
        a = anim * 0.12 + k;
        vec_glow_size( 0 );
        if( z > 30 ) vec_glow_size( 10 );
        vec_color( 0xFFFFFF80, 0xFF505010 );
        vec_intensity( world_intensity * depth_fade( z ) );
        for( j = 0; j < 4; j++ )
          vec_line( x + cos( a + j * 1.5707963 ) * s, y + sin( a + j * 1.5707963 ) * s * 1.3,
                    x + cos( a + ( j + 1 ) * 1.5707963 ) * s, y + sin( a + ( j + 1 ) * 1.5707963 ) * s * 1.3 );
        vec_line( x - s * 0.35, y, x + s * 0.35, y );
    }
    vec_intensity( world_intensity );
}

float ck_ox = 0, ck_oy = 0;       // the cockpit's sway this frame
int laser_timer = 0;              // the bolts' flight, in frames
int fire_cooldown = 0;
int target_type = TG_NONE;
int target_index = 0;
float hit_flash_x = 0, hit_flash_y = 0;
int hit_flash = 0;

int wave_hits = 0;                // shields lost this wave
int trench_shots = 0;             // shots fired in this trench run
int turrets_built = 0, turrets_killed = 0;
int bombers_killed = 0;
int dreadnoughts = 0;
int time_left = 0;                // time attack, in frames

void add_score( int points )
{
    if( game_mode == MODE_LAST ) points *= 2;
    score += points;
    if( game_mode == MODE_STANDARD && score >= 200000 ) award_feat( FEAT_ACE );
    if( score > high_score ) high_score = score;
}

// how close the crosshair is to a camera-relative point, against its size
float best_z;

void consider( float x, float y, float z, float radius, int type, int index )
{
    float sx, sy, r, dx, dy;
    if( z < 3 || z > FAR ) return;
    if( !vec_project( x, y, z, &sx, &sy ) ) return;
    r = fmax( 11, radius * FOCAL / z );
    dx = sx - aim_x;  dy = sy - aim_y;
    if( dx * dx + dy * dy > r * r ) return;
    if( z >= best_z ) return;
    best_z = z;
    target_type = type;
    target_index = index;
}

// finds what the crosshair is over (the nearest thing), or TG_NONE
void find_target()
{
    int i;
    float z;
    target_type = TG_NONE;
    best_z = 100000.0;

    for( i = 0; i < MAX_FB; i++ )
      if( fb_active[ i ] ) consider( fb_x[ i ] - ship_x, fb_y[ i ] - ship_y, fb_z[ i ], 2.2, TG_FIREBALL, i );

    if( phase == PH_SPACE )
    {
        for( i = 0; i < MAX_FIGHTERS; i++ )
          if( ft_active[ i ] ) consider( ft_x[ i ], ft_y[ i ], ft_z[ i ], 6.0, TG_FIGHTER, i );
        for( i = 0; i < MAX_BOMBERS; i++ )
          if( bb_active[ i ] ) consider( bb_x[ i ], bb_y[ i ], bb_z[ i ], 9.0, TG_BOMBER, i );
        for( i = 0; i < MAX_MINES; i++ )
          if( mn_active[ i ] ) consider( mn_x[ i ], mn_y[ i ], mn_z[ i ], 3.0, TG_MINE, i );
    }

    if( phase == PH_HULL )
    {
        for( i = 0; i < MAX_TOWERS; i++ )
          if( tw_active[ i ] ) consider( tw_x[ i ] - ship_x, HULL_Y - tw_h[ i ] * 0.6 - ship_y, tw_z[ i ], 5.5, TG_TOWER, i );
        for( i = 0; i < MAX_GENS; i++ )
          if( gn_active[ i ] ) consider( gn_x[ i ] - ship_x, HULL_Y - 3.5 - ship_y, gn_z[ i ], 7.0, TG_GEN, i );
    }

    if( phase == PH_TRENCH )
    {
        for( i = 0; i < MAX_TURRETS; i++ )
          if( tu_active[ i ] )
            consider( tu_side[ i ] * ( TRENCH_W - 1.5 ) - ship_x, tu_y[ i ] - ship_y, tu_pos[ i ] - trench_pos, 2.6, TG_TURRET, i );
        z = trench_len - trench_pos;
        if( z < 150 ) consider( -ship_x, TRENCH_FLOOR - ship_y, z, 4.0, TG_VENT, 0 );
    }

    if( phase == PH_CORE )
      for( i = 0; i < MAX_CELLS; i++ )
        if( ce_active[ i ] ) consider( ce_x[ i ] - ship_x, ce_y[ i ] - ship_y, ce_pos[ i ] - core_pos, 2.6, TG_CELL, i );
}

void start_phase( int p );

void fire_lasers()
{

    int i;

    laser_timer = 7;
    sfx( SND_LASER, 0.35 );
    if( phase == PH_TRENCH ) trench_shots++;
    find_target();
    if( target_type == TG_NONE ) return;

    i = target_index;
    hit_flash = 8;
    hit_flash_x = aim_x;
    hit_flash_y = aim_y;

    if( target_type == TG_FIREBALL )
    {
        fb_active[ i ] = false;
        add_score( 100 );
        sfx( SND_FB_POP, 0.45 );
        blow_up( fb_x[ i ], fb_y[ i ], fb_z[ i ], 6, 0.35, 1.2, 0xFF40B0FF );
    }
    else if( target_type == TG_FIGHTER )
    {
        ft_active[ i ] = false;
        kills++;
        add_score( 1000 );
        sfx( SND_EXPLODE, 0.6 );
        blow_up( ft_x[ i ], ft_y[ i ], ft_z[ i ], 14, 0.5, 2.6, 0xFF80FF80 );
        add_shake( 2 );
    }
    else if( target_type == TG_TOWER )
    {
        tw_active[ i ] = false;
        towers_killed++;
        add_score( 500 );
        sfx( SND_TOWER, 0.6 );
        blow_up( tw_x[ i ], HULL_Y - tw_h[ i ], tw_z[ i ], 14, 0.45, 2.4, 0xFF60E0FF );
    }
    else if( target_type == TG_TURRET )
    {
        tu_active[ i ] = false;
        turrets_killed++;
        add_score( 300 );
        sfx( SND_EXPLODE, 0.55 );
        blow_up( tu_side[ i ] * ( TRENCH_W - 1.5 ), tu_y[ i ], tu_pos[ i ] - trench_pos, 10, 0.35, 1.8, 0xFF60E0FF );
    }
    else if( target_type == TG_BOMBER )
    {
        bb_hp[ i ]--;
        bb_hit[ i ] = 6;
        if( bb_hp[ i ] > 0 ) sfx( SND_FB_POP, 0.45 );
        else
        {
            bb_active[ i ] = false;
            bombers_killed++;
            if( bombers_killed >= 5 ) award_feat( FEAT_BOMBERS );
            add_score( 2500 );
            sfx( SND_TOWER, 0.65 );
            blow_up( bb_x[ i ], bb_y[ i ], bb_z[ i ], 18, 0.55, 3.2, 0xFFFF70FF );
            add_shake( 3 );
        }
    }
    else if( target_type == TG_MINE )
    {
        mn_active[ i ] = false;
        add_score( 300 );
        sfx( SND_FB_POP, 0.5 );
        blow_up( mn_x[ i ], mn_y[ i ], mn_z[ i ], 8, 0.4, 1.6, 0xFF4848FF );
    }
    else if( target_type == TG_GEN )
    {
        gn_active[ i ] = false;
        gens_destroyed++;
        add_score( 2000 );
        sfx( SND_TOWER, 0.65 );
        sfx( SND_EXTRA, 0.4 );
        blow_up( gn_x[ i ], HULL_Y - 4, gn_z[ i ], 16, 0.5, 2.6, 0xFFFFC0A0 );
        banner_text = "GENERATOR DOWN";
        banner_sub = "FEWER TURRETS IN THE TRENCH";
        banner_timer = 100;
    }
    else if( target_type == TG_CELL )
    {
        ce_active[ i ] = false;
        cells_hit++;
        add_score( 500 );
        sfx_pitched( SND_FB_POP, 0.5, 1.0 + cells_hit * 0.04 );
        blow_up( ce_x[ i ], ce_y[ i ], ce_pos[ i ] - core_pos, 8, 0.4, 1.6, 0xFFFFFF80 );
    }
    else if( target_type == TG_VENT )
    {
        vent_hit = true;
        dreadnoughts++;
        award_feat( FEAT_DREADNOUGHT );
        if( dreadnoughts >= 3 && game_mode == MODE_STANDARD ) award_feat( FEAT_FLEET );
        if( wave_hits == 0 ) award_feat( FEAT_FLAWLESS );
        if( turrets_built > 0 && turrets_killed >= turrets_built ) award_feat( FEAT_SWEEP );
        if( trench_shots == 1 )
        {
            // the only shot fired in the trench: the way inside opens
            award_feat( FEAT_INSTINCT );
            start_phase( PH_CORE );
            return;
        }
        start_phase( PH_BOOM );
    }
}

void draw_lasers()
{
    float p, q, gl, gr, gy;
    if( laser_timer <= 0 ) return;
    p = ( 7 - laser_timer ) / 7.0;
    q = fmin( 1.0, p + 0.4 );
    vec_thickness( 3 );
    vec_glow_size( 16 );
    vec_color( 0xFF6060FF, 0xFF201880 );
    gl = 40 + ck_ox;  gr = 600 + ck_ox;  gy = 344 + ck_oy;
    vec_line( gl + ( aim_x - gl ) * p, gy + ( aim_y - gy ) * p, gl + ( aim_x - gl ) * q, gy + ( aim_y - gy ) * q );
    vec_line( gr + ( aim_x - gr ) * p, gy + ( aim_y - gy ) * p, gr + ( aim_x - gr ) * q, gy + ( aim_y - gy ) * q );
}

// -----------------------------------------------------------------------------
//   COCKPIT, CRACKED GLASS AND WRECKAGE (visual only: everything here uses the
//   effects random numbers, so a replay plays out the same)
// -----------------------------------------------------------------------------

#define MAX_CRACKS 6
#define CRACK_SEGS 28
#define CK_TOTAL   168

float[ CK_TOTAL ] ck_x1;
float[ CK_TOTAL ] ck_y1;
float[ CK_TOTAL ] ck_x2;
float[ CK_TOTAL ] ck_y2;
int[ MAX_CRACKS ] ck_segs;
int[ MAX_CRACKS ] ck_age;
int crack_count = 0;
int repair_timer = 0;             // a crack sealing up as a shield comes back
float[ 8 ] ring_x;
float[ 8 ] ring_y;

void crack_seg( int c, float x1, float y1, float x2, float y2 )
{
    int i;
    if( ck_segs[ c ] >= CRACK_SEGS ) return;
    i = c * CRACK_SEGS + ck_segs[ c ];
    ck_x1[ i ] = x1;  ck_y1[ i ] = y1;
    ck_x2[ i ] = x2;  ck_y2[ i ] = y2;
    ck_segs[ c ]++;
}

void copy_crack( int from, int to )
{
    int k, a, b;
    for( k = 0; k < CRACK_SEGS; k++ )
    {
        a = from * CRACK_SEGS + k;
        b = to * CRACK_SEGS + k;
        ck_x1[ b ] = ck_x1[ a ];  ck_y1[ b ] = ck_y1[ a ];
        ck_x2[ b ] = ck_x2[ a ];  ck_y2[ b ] = ck_y2[ a ];
    }
    ck_segs[ to ] = ck_segs[ from ];
    ck_age[ to ] = ck_age[ from ];
}

// a star of cracks in the canopy where a hit landed
void add_crack()
{
    int c, k, j, rays;
    float cx, cy, a, len, x, y, nx, ny, r;
    if( crack_count >= MAX_CRACKS )
    {
        for( c = 1; c < MAX_CRACKS; c++ ) copy_crack( c, c - 1 );
        crack_count = MAX_CRACKS - 1;
    }
    c = crack_count;
    crack_count++;
    repair_timer = 0;
    ck_segs[ c ] = 0;
    ck_age[ c ] = 0;

    // off to one side of the glass, clear of the middle
    cx = fx_range( 70, 230 );
    if( fx_range( 0, 1 ) < 0.5 ) cx = 640 - cx;
    cy = fx_range( 80, 280 );

    rays = 5 + fx_range( 0, 2.99 );
    for( k = 0; k < rays; k++ )
    {
        a = k * 6.2831853 / rays + fx_range( -0.3, 0.3 );
        len = fx_range( 30, 72 );
        r = fx_range( 3, 6 );
        x = cx + cos( a ) * r;
        y = cy + sin( a ) * r;
        for( j = 0; j < 3; j++ )
        {
            a += fx_range( -0.35, 0.35 );
            nx = x + cos( a ) * len / 3;
            ny = y + sin( a ) * len / 3;
            crack_seg( c, x, y, nx, ny );
            x = nx;  y = ny;
            if( j == 0 ) { ring_x[ k ] = x;  ring_y[ k ] = y; }
        }
    }
    // the fracture ring round the impact
    for( k = 0; k < rays; k++ )
      if( fx_range( 0, 1 ) < 0.7 )
        crack_seg( c, ring_x[ k ], ring_y[ k ], ring_x[ ( k + 1 ) % rays ], ring_y[ ( k + 1 ) % rays ] );
}

// a shield comes back: the newest crack seals
void repair_crack()
{
    if( crack_count <= 0 ) return;
    crack_count--;
    repair_timer = 50;
}

void draw_one_crack( int c, float f )
{
    int k, i;
    vec_intensity( f );
    for( k = 0; k < ck_segs[ c ]; k++ )
    {
        i = c * CRACK_SEGS + k;
        vec_line( ck_x1[ i ], ck_y1[ i ], ck_x2[ i ], ck_y2[ i ] );
    }
}

void draw_cracks()
{
    int c;
    float f;
    vec_thickness( 1.3 );
    for( c = 0; c < crack_count; c++ )
    {
        vec_glow_size( 0 );
        vec_color( 0xFFFFF4E8, 0 );
        f = 0.38;
        if( ck_age[ c ] < 40 )
        {
            // fresh: bright, then settling to a faint scar
            f = 1.0 - 0.62 * ck_age[ c ] / 40.0;
            vec_glow_size( 8 );
            vec_color( 0xFFFFF4E8, 0xFF605048 );
        }
        draw_one_crack( c, f );
    }
    if( repair_timer > 0 )
    {
        vec_glow_size( 8 );
        vec_color( 0xFFFFFFC0, 0xFF606030 );
        draw_one_crack( crack_count, repair_timer / 50.0 * 0.7 );
    }
    vec_intensity( 1 );
}

int cockpit_color = 0xFF907868;

// canopy struts across the top corners, and the gun housings low in the corners
void draw_cockpit()
{
    int k;
    float a, f = 0.75;
    if( paused ) f = 0.35;
    vec_thickness( 2.0 );
    vec_glow_size( 0 );
    vec_color( cockpit_color, 0 );
    if( invuln > 26 && ( invuln / 3 ) % 2 == 0 ) vec_color( 0xFF5050FF, 0 );
    vec_intensity( f );

    vec_line( -10, 160, 160, -10 );
    vec_line( -10, 176, 176, -10 );
    vec_line( 650, 160, 480, -10 );
    vec_line( 650, 176, 464, -10 );
    for( k = 0; k < 3; k++ )
    {
        a = 30 + k * 40;
        vec_line( a, 150 - a, a + 8, 158 - a );
        vec_line( 640 - a, 150 - a, 632 - a, 158 - a );
    }

    vec_line( -10, 316, 28, 330 );
    vec_line( 28, 330, 62, 330 );
    vec_line( 62, 330, 84, 370 );
    vec_line( 650, 316, 612, 330 );
    vec_line( 612, 330, 578, 330 );
    vec_line( 578, 330, 556, 370 );

    // the muzzles flare as the bolts leave
    vec_thickness( 1.8 );
    if( laser_timer > 3 )
    {
        vec_glow_size( 10 );
        vec_color( 0xFF8080FF, 0xFF202090 );
        vec_intensity( 1 );
    }
    vec_line( 32, 344, 48, 344 );
    vec_line( 32, 344, 29, 366 );
    vec_line( 48, 344, 51, 366 );
    vec_line( 592, 344, 608, 344 );
    vec_line( 592, 344, 589, 366 );
    vec_line( 608, 344, 611, 366 );
    vec_intensity( 1 );
}

// wreckage: tumbling slabs of hull flung out when the dreadnought goes up
#define MAX_CHUNKS 6
float[ MAX_CHUNKS ] ch_x;
float[ MAX_CHUNKS ] ch_y;
float[ MAX_CHUNKS ] ch_z;
float[ MAX_CHUNKS ] ch_vx;
float[ MAX_CHUNKS ] ch_vy;
float[ MAX_CHUNKS ] ch_vz;
float[ MAX_CHUNKS ] ch_yaw;
float[ MAX_CHUNKS ] ch_roll;
float[ MAX_CHUNKS ] ch_dyaw;
float[ MAX_CHUNKS ] ch_droll;
float[ MAX_CHUNKS ] ch_w;
float[ MAX_CHUNKS ] ch_h;
float[ MAX_CHUNKS ] ch_d;
int[ MAX_CHUNKS ] ch_life;

void spawn_chunks( float x, float y, float z )
{
    int i;
    for( i = 0; i < MAX_CHUNKS; i++ )
    {
        ch_x[ i ] = x + fx_range( -24, 24 );
        ch_y[ i ] = y + fx_range( -8, 8 );
        ch_z[ i ] = z + fx_range( -10, 10 );
        ch_vx[ i ] = ( ch_x[ i ] - x ) * 0.08 + fx_range( -1.2, 1.2 );
        ch_vy[ i ] = ( ch_y[ i ] - y ) * 0.1 + fx_range( -1.0, 0.8 );
        ch_vz[ i ] = fx_range( -1.7, 0.5 );
        ch_yaw[ i ] = fx_range( 0, 6.28 );
        ch_roll[ i ] = fx_range( 0, 6.28 );
        ch_dyaw[ i ] = fx_range( -0.07, 0.07 );
        ch_droll[ i ] = fx_range( -0.09, 0.09 );
        ch_w[ i ] = fx_range( 2, 6 );
        ch_h[ i ] = fx_range( 0.8, 2.4 );
        ch_d[ i ] = fx_range( 3, 8 );
        ch_life[ i ] = 175;
    }
}

void clear_glass()
{
    int i;
    crack_count = 0;
    repair_timer = 0;
    for( i = 0; i < MAX_CHUNKS; i++ ) ch_life[ i ] = 0;
}

void update_glass()
{
    int i;
    for( i = 0; i < crack_count; i++ )
      if( ck_age[ i ] < 1000 ) ck_age[ i ]++;
    if( repair_timer > 0 ) repair_timer--;
    for( i = 0; i < MAX_CHUNKS; i++ )
      if( ch_life[ i ] > 0 )
      {
          ch_life[ i ]--;
          ch_x[ i ] += ch_vx[ i ];
          ch_y[ i ] += ch_vy[ i ];
          ch_z[ i ] += ch_vz[ i ];
          ch_yaw[ i ] += ch_dyaw[ i ];
          ch_roll[ i ] += ch_droll[ i ];
          if( ch_z[ i ] < 3 ) ch_life[ i ] = 0;
      }
}

void draw_chunks()
{
    int i;
    float w, h, d, wi = world_intensity;
    vec_thickness( 1.8 );
    vec_glow_size( 6 );
    for( i = 0; i < MAX_CHUNKS; i++ )
      if( ch_life[ i ] > 0 )
      {
          if( get_cycle_counter() > DEBRIS_CYCLE_BUDGET ) break;
          w = ch_w[ i ];  h = ch_h[ i ];  d = ch_d[ i ];
          mv_x[ 0 ] = -w; mv_y[ 0 ] = -h; mv_z[ 0 ] = -d;
          mv_x[ 1 ] =  w; mv_y[ 1 ] = -h; mv_z[ 1 ] = -d;
          mv_x[ 2 ] =  w; mv_y[ 2 ] = -h; mv_z[ 2 ] =  d;
          mv_x[ 3 ] = -w; mv_y[ 3 ] = -h; mv_z[ 3 ] =  d;
          mv_x[ 4 ] = -w; mv_y[ 4 ] =  h; mv_z[ 4 ] = -d;
          mv_x[ 5 ] =  w; mv_y[ 5 ] =  h; mv_z[ 5 ] = -d;
          mv_x[ 6 ] =  w; mv_y[ 6 ] =  h; mv_z[ 6 ] =  d;
          mv_x[ 7 ] = -w; mv_y[ 7 ] =  h; mv_z[ 7 ] =  d;
          vec_color( class_core(), vec_scale_rgb( class_core(), 0.3 ) );
          world_intensity = wi * fmin( 1.0, ch_life[ i ] / 40.0 );
          draw_model( mv_x, mv_y, mv_z, 8, tower_edges, 12, ch_x[ i ] - ship_x, ch_y[ i ] - ship_y, ch_z[ i ],
                      ch_yaw[ i ], ch_roll[ i ], w + h + d );
      }
    world_intensity = wi;
    vec_intensity( wi );
}

// -----------------------------------------------------------------------------
//   SHIELDS
// -----------------------------------------------------------------------------

void shield_hit()
{
    if( invuln > 0 || p_dead || phase == PH_BOOM ) return;
    wave_hits++;
    if( game_mode == MODE_TIME )
    {
        // time attack: no shields to lose, but every hit costs time
        time_left = max( 0, time_left - 600 );
        invuln = 40;
        add_crack();
        add_shake( 9 );
        add_flash( 0.9, 0xFF3030FF );
        sfx( SND_SHIELD, 0.7 );
        return;
    }
    shields--;
    invuln = 40;
    add_crack();
    add_shake( 9 );
    add_flash( 0.9, 0xFF3030FF );
    sfx( SND_SHIELD, 0.7 );
    if( shields <= 0 )
    {
        shields = 0;
        p_dead = true;
        dead_timer = 0;
        sfx( SND_BIG_BOOM, 0.6 );
        blow_up( 0, 0, 12, 20, 0.6, 3.0, 0xFF6060FF );
        add_crack();
        add_crack();
    }
}

// -----------------------------------------------------------------------------
//   PHASES
// -----------------------------------------------------------------------------

int boom_timer = 0;
int boom_bonus = 0;

void clear_objects()
{
    int i;
    for( i = 0; i < MAX_FB; i++ ) fb_active[ i ] = false;
    for( i = 0; i < MAX_FIGHTERS; i++ ) ft_active[ i ] = false;
    for( i = 0; i < MAX_TOWERS; i++ ) tw_active[ i ] = false;
    for( i = 0; i < MAX_CATWALKS; i++ ) cw_active[ i ] = false;
    for( i = 0; i < MAX_TURRETS; i++ ) tu_active[ i ] = false;
    for( i = 0; i < MAX_PLATES; i++ ) pl_active[ i ] = false;
    for( i = 0; i < MAX_BOMBERS; i++ ) bb_active[ i ] = false;
    for( i = 0; i < MAX_MINES; i++ ) mn_active[ i ] = false;
    for( i = 0; i < MAX_GENS; i++ ) gn_active[ i ] = false;
    for( i = 0; i < MAX_GATES; i++ ) lg_active[ i ] = false;
}

// a fresh trench run: count its turrets, and the shots fired in it
void count_trench()
{
    int i;
    turrets_built = 0;
    turrets_killed = 0;
    trench_shots = 0;
    for( i = 0; i < MAX_TURRETS; i++ )
      if( tu_active[ i ] ) turrets_built++;
}

void start_phase( int p )
{
    phase = p;
    phase_timer = 0;
    banner_timer = 160;
    clear_objects();

    if( p == PH_SPACE )
    {
        banner_text = "INTERCEPTORS";
        banner_sub = "SHOOT THEM DOWN - SHOOT THEIR FIREBALLS";
        if( wave > 1 ) banner_sub = class_name();
        bomber_timer = 300;
        mine_timer = 400;
        ship_x = 0;  ship_y = 0;
        kills = 0;
        kill_quota = min( 6 + 2 * ( wave - 1 ), 16 );
        spawn_timer = 50;
        dn_x = 320;  dn_y = 150;
        sfx( SND_BANNER, 0.5 );
    }
    else if( p == PH_HULL )
    {
        banner_text = "ATTACK THE HULL";
        banner_sub = "SHOOT THE GUN TOWERS";
        ship_x = 0;  ship_y = 0;
        towers_spawned = 0;
        towers_killed = 0;
        towers_quota = min( 8 + 2 * wave, 20 );
        tower_timer = 60;
        hull_travel = 0;
        gens_spawned = 0;
        gens_destroyed = 0;
        gens_quota = 0;
        if( wave >= 2 ) gens_quota = min( 1 + wave / 2, 3 );
        if( wave >= 2 && dn_class() == CLASS_BASTION ) gens_quota++;
        gen_timer = 140;
        if( gens_quota > 0 ) banner_sub = "SHOOT THE TOWERS AND SHIELD GENERATORS";
        sfx( SND_BANNER, 0.5 );
    }
    else if( p == PH_CORE )
    {
        banner_text = "YOU FOUND A WAY INSIDE";
        banner_sub = "SHOOT EVERY ENERGY CELL";
        banner_timer = 200;
        ship_x = 0;  ship_y = 0;
        build_core();
        sfx( SND_BANNER, 0.5 );
        sfx( SND_EXTRA, 0.5 );
        add_flash( 0.7, 0xFFFF60C0 );
    }
    else if( p == PH_TRENCH )
    {
        banner_text = "ENTER THE TRENCH";
        banner_sub = "HIT THE REACTOR VENT AT THE END";
        ship_x = 0;  ship_y = 0;
        trench_pos = 0;
        trench_clock = 0;
        trench_len = 2200 + 300 * wave;
        vent_hit = false;
        build_trench();
        count_trench();
        sfx( SND_BANNER, 0.5 );
    }
    else
    {
        // the vent is hit: the dreadnought goes up
        banner_text = "DREADNOUGHT DESTROYED";
        banner_sub = "";
        boom_timer = 0;
        boom_bonus = 25000 + shields * 2000;
        add_score( boom_bonus );
        sfx( SND_BIG_BOOM, 0.6 );
        add_flash( 0.7, 0xFF40A0FF );
        add_shake( 8 );
        banner_timer = 0;          // shown when it finally blows
    }
}

void update_space()
{
    scroll = 0;
    update_stars( 2.2 );
    update_fighters();
    update_bombers();
    update_mines();
    dn_x += ( 320 - dn_x ) * 0.004 - turn_x * FOCAL;
    dn_y += ( 150 - dn_y ) * 0.004 - turn_y * FOCAL;
    if( kills >= kill_quota && count_fighters() == 0 && phase_timer > 60 ) start_phase( PH_HULL );
    // the last interceptors leave once the quota is met
    if( kills >= kill_quota ) clear_objects();
}

void update_hull()
{
    float target = ( aim_x - SCREEN_CX ) / 320.0 * 60;
    scroll = hull_speed();
    hull_travel += scroll;
    ship_x += ( target - ship_x ) * 0.05;
    update_plates();
    update_towers();
    update_gens();
    if( towers_spawned >= towers_quota && count_towers() == 0 && gens_spawned >= gens_quota && count_gens() == 0 )
    {
        if( towers_killed >= towers_quota )
        {
            add_score( 5000 );
            banner_text = "ALL TOWERS DOWN";
            banner_sub = "BONUS 5000";
            banner_timer = 120;
            sfx( SND_EXTRA, 0.5 );
        }
        start_phase( PH_TRENCH );
    }
}

void update_trench_phase()
{
    float tx = ( aim_x - SCREEN_CX ) / 300.0 * ( TRENCH_W - 2.5 );
    float ty = ( aim_y - SCREEN_CY ) / 160.0 * ( TRENCH_FLOOR - 2.5 );
    scroll = trench_speed();
    ship_x += ( clampf( tx, -TRENCH_W + 2.5, TRENCH_W - 2.5 ) - ship_x ) * 0.12;
    ship_y += ( clampf( ty, TRENCH_TOP + 2.5, TRENCH_FLOOR - 2.5 ) - ship_y ) * 0.12;
    update_trench();

    if( trench_len - trench_pos < 2 && !vent_hit )
    {
        // missed: go round for another run at the vent
        trench_pos = trench_len - 1500;
        build_trench();
        count_trench();
        banner_text = "VENT MISSED";
        banner_sub = "GOING ROUND FOR ANOTHER RUN";
        banner_timer = 140;
        sfx( SND_BANNER, 0.5 );
    }
}

// the dreadnought's end: fires in the trench as you pull up and out, the ship
// seen whole and breaking up, then one huge blast flinging wreckage
#define BOOM_BLAST 125

float boom_dn_y()
{
    float e = 1 - clampf( ( boom_timer - 30 ) / 55.0, 0, 1 );
    return 175 + 280 * e * e * e;
}

float boom_dn_scale()
{
    return 1.5 - boom_timer * 0.0015;
}

void update_boom()
{
    float px, py, z;
    scroll = 0;
    boom_timer++;
    update_stars( 6 );
    if( boom_timer < 36 && boom_timer % 4 == 0 )
      blow_up( fx_range( -TRENCH_W, TRENCH_W ), fx_range( -4, 10 ), fx_range( 25, 70 ), 5, 0.8, 3.0, 0xFF40A0FF );
    if( boom_timer >= 45 && boom_timer < BOOM_BLAST && boom_timer % 5 == 0 )
    {
        z = 110;
        px = fx_range( -130, 130 ) * boom_dn_scale();
        py = boom_dn_y() - SCREEN_CY + fx_range( -18, 18 ) * boom_dn_scale();
        blow_up( px * z / FOCAL + ship_x, py * z / FOCAL + ship_y, z, 6, 0.7, 3.5, 0xFF40A0FF );
        sfx_pitched( SND_EXPLODE, 0.3, fx_range( 0.6, 1.0 ) );
        add_flash( 0.1, 0xFF40A0FF );
        add_shake( 2 );
    }
    if( boom_timer == BOOM_BLAST )
    {
        sfx( SND_BIG_BOOM, 0.9 );
        add_flash( 1.0, 0xFFFFFFFF );
        add_shake( 16 );
        z = 120;
        py = ( boom_dn_y() - SCREEN_CY ) * z / FOCAL + ship_y;
        blow_up( ship_x, py, z, 16, 1.8, 7.0, 0xFFFFFFFF );
        spawn_chunks( ship_x, py, z );
        banner_timer = 175;
    }
    if( boom_timer > 300 )
    {
        wave++;
        wave_hits = 0;
        from_core = false;
        if( game_mode == MODE_TIME )
        {
            repair_crack();
            time_left += 1800;
        }
        else if( shields < max_shields && game_mode != MODE_LAST )
        {
            repair_crack();
            shields++;
            sfx( SND_EXTRA, 0.55 );
        }
        if( game_mode == MODE_TRENCH )
        {
            gens_destroyed = 0;
            start_phase( PH_TRENCH );
            banner_text = "NEXT TRENCH";
            banner_sub = class_name();
        }
        else
        {
            start_phase( PH_SPACE );
            banner_text = "NEXT WAVE";
        }
        if( game_mode == MODE_TIME ) banner_sub = "PLUS 30 SECONDS";
    }
}

// -----------------------------------------------------------------------------
//   GAME FLOW
// -----------------------------------------------------------------------------

// resets every piece of gameplay state, then seeds the gameplay RNG:
// a live game and its replay must start out bit-for-bit identical
void begin_game( int seed, bool demo )
{
    int i;
    game_difficulty = opt_difficulty;
    if( demo ) game_difficulty = demo_difficulty;

    game_seed = seed;
    rng_state = seed;
    fx_state = seed ^ 0x2545F491;
    if( fx_state == 0 ) fx_state = 1;

    demo_mode = demo;
    sfx_muted = demo;
    demo_pos = 0;
    rec_frames = 0;
    for( i = 0; i < REC_WORDS; i++ ) rec_data[ i ] = 0;
    in_bits = 0;
    input_lock = false;
    save_message = SAVE_MSG_NONE;
    high_score = hs_score[ 0 ];

    score = 0;
    wave = 1;
    max_shields = diff_pick( 8, 6, 4 );
    if( game_mode == MODE_TRENCH ) max_shields = diff_pick( 4, 3, 2 );
    if( game_mode == MODE_LAST ) max_shields = 1;
    shields = max_shields;
    time_left = 180 * 60;
    wave_hits = 0;
    bombers_killed = 0;
    dreadnoughts = 0;
    from_core = false;
    gens_destroyed = 0;
    toast_pending = 0;
    toast_feat = -1;
    toast_timer = 0;
    invuln = 0;
    p_dead = false;
    dead_timer = 0;
    game_frame = 0;
    aim_x = SCREEN_CX;  aim_y = SCREEN_CY;
    aim_vx = 0;  aim_vy = 0;
    turn_x = 0;  turn_y = 0;
    laser_timer = 0;
    fire_cooldown = 0;
    hit_flash = 0;
    shake = 0;
    flash = 0;
    paused = false;
    for( i = 0; i < MAX_DEBRIS; i++ ) db_life[ i ] = 0;
    clear_glass();
    init_stars();
    state = STATE_PLAYING;
    if( demo ) state = STATE_DEMO;
    state_timer = 0;
    if( game_mode == MODE_TRENCH )
    {
        start_phase( PH_TRENCH );
        banner_text = "TRENCH RUN";
    }
    else
    {
    start_phase( PH_SPACE );
    banner_text = "WAVE 1";
    }
    if( game_mode == MODE_TIME ) banner_sub = "3 MINUTES - EVERY HIT COSTS 10 SECONDS";
    if( game_mode == MODE_LAST ) banner_sub = "ONE SHIELD - DOUBLE POINTS";
    sfx( SND_START, 0.5 );
}

void new_game()
{
    // the hardware RNG (seeded from the clock) picks the game's seed
    begin_game( rand() % 2000000000 + 1, false );
}

// -----------------------------------------------------------------------------
//   TITLE PAGES, GAME END, INITIALS ENTRY
// -----------------------------------------------------------------------------

#define PAGE_LOGO    0
#define PAGE_SCORES  1
#define PAGE_RECORD  2
int title_page = PAGE_LOGO;
int highlight_rank = -1;

void enter_title_page( int page )
{
    clear_objects();
    p_dead = false;
    demo_mode = false;
    sfx_muted = false;
    paused = false;
    phase = PH_SPACE;
    ship_x = 0;  ship_y = 0;
    turn_x = 0;  turn_y = 0;
    scroll = 0;
    shake = 0;
    flash = 0;
    title_page = page;
    state = STATE_TITLE;
    state_timer = 0;
}

void enter_title()
{
    highlight_rank = -1;
    enter_title_page( PAGE_LOGO );
}

int* entry_charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789- ";
#define ENTRY_CHARS 38
int[ 3 ] entry_index;
int entry_cursor;
int entry_rank;
int entry_mode = MODE_STANDARD;   // which table the initials go to

bool pressed_or_repeat( int held_frames )
{
    return held_frames == 1 || ( held_frames > 18 && held_frames % 5 == 0 );
}

// a live game ended: keep its recording if it's the best so far, then ask
// for initials or show the game over screen
void finish_game()
{
    bool demo_changed = false;

    play_track( MUSIC_GAMEOVER, 0.5 );
    entry_mode = game_mode;

    // the modes keep a best each, and record no demos
    if( game_mode != MODE_STANDARD )
    {
        if( score > mode_best[ game_mode ] )
        {
            entry_index[ 0 ] = 0;
            entry_index[ 1 ] = 0;
            entry_index[ 2 ] = 0;
            entry_cursor = 0;
            entry_rank = -1;
            sfx( SND_EXTRA, 0.6 );
            state = STATE_ENTRY;
            state_timer = 0;
            return;
        }
        save_message = SAVE_MSG_NONE;
        if( progress_dirty ) save_message = save_all();
        state = STATE_GAMEOVER;
        state_timer = 0;
        return;
    }

    if( score > demo_score && rec_frames >= DEMO_MIN_FRAMES )
    {
        memcpy( demo_data, rec_data, REC_WORDS );
        demo_seed = game_seed;
        demo_frames = rec_frames;
        demo_score = score;
        demo_difficulty = game_difficulty;
        demo_changed = true;
    }

    if( table_rank( score ) >= 0 )
    {
        entry_index[ 0 ] = 0;
        entry_index[ 1 ] = 0;
        entry_index[ 2 ] = 0;
        entry_cursor = 0;
        entry_rank = table_rank( score );
        sfx( SND_EXTRA, 0.6 );
        state = STATE_ENTRY;
        state_timer = 0;
        return;
    }

    save_message = SAVE_MSG_NONE;
    if( demo_changed || progress_dirty ) save_message = save_all();
    state = STATE_GAMEOVER;
    state_timer = 0;
    if( opt_music == 0 ) sfx( SND_GAME_OVER, 0.5 );
}

void enter_modes();

void finish_entry()
{
    int[ 4 ] name;
    name[ 0 ] = entry_charset[ entry_index[ 0 ] ];
    name[ 1 ] = entry_charset[ entry_index[ 1 ] ];
    name[ 2 ] = entry_charset[ entry_index[ 2 ] ];
    name[ 3 ] = 0;

    if( entry_mode != MODE_STANDARD )
    {
        mode_best[ entry_mode ] = score;
        mode_name[ entry_mode ] = pack_name( name ) | ( game_difficulty << 24 );
        save_message = save_all();
        sfx( SND_START, 0.6 );
        chosen_mode = entry_mode;
        enter_modes();
        return;
    }

    table_insert( score, pack_name( name ) | ( game_difficulty << 24 ), entry_rank );
    high_score = hs_score[ 0 ];
    save_message = save_all();
    sfx( SND_START, 0.6 );
    highlight_rank = entry_rank;
    enter_title_page( PAGE_SCORES );
}

void update_entry()
{
    int i = entry_cursor;

    if( pressed_or_repeat( gamepad_up() ) )
    {
        entry_index[ i ] = ( entry_index[ i ] + 1 ) % ENTRY_CHARS;
        sfx( SND_BEEP, 0.35 );
    }
    if( pressed_or_repeat( gamepad_down() ) )
    {
        entry_index[ i ] = ( entry_index[ i ] + ENTRY_CHARS - 1 ) % ENTRY_CHARS;
        sfx( SND_BEEP, 0.35 );
    }
    if( gamepad_left() == 1 && entry_cursor > 0 ) entry_cursor--;
    if( gamepad_right() == 1 && entry_cursor < 2 ) entry_cursor++;

    if( gamepad_button_b() == 1 || gamepad_button_a() == 1 )
    {
        if( entry_cursor < 2 )
        {
            entry_cursor++;
            sfx( SND_LASER, 0.3 );
        }
        else
        {
            finish_entry();
            return;
        }
    }

    if( gamepad_button_start() == 1 || state_timer > 1800 ) finish_entry();
}

// -----------------------------------------------------------------------------
//   MODE SELECT
// -----------------------------------------------------------------------------

void enter_modes()
{
    enter_title_page( PAGE_LOGO );
    state = STATE_MODES;
    state_timer = 0;
    input_lock = true;
}

// Start from the title: straight into a game, or to the modes once there are any
void title_start()
{
    if( any_mode_unlocked() ) enter_modes();
    else
    {
        game_mode = MODE_STANDARD;
        new_game();
    }
}

void update_modes()
{
    if( state_timer < 10 ) return;
    if( pressed_or_repeat( gamepad_up() ) )
    {
        chosen_mode = ( chosen_mode + MODES - 1 ) % MODES;
        sfx( SND_BEEP, 0.35 );
    }
    if( pressed_or_repeat( gamepad_down() ) )
    {
        chosen_mode = ( chosen_mode + 1 ) % MODES;
        sfx( SND_BEEP, 0.35 );
    }
    if( gamepad_button_b() == 1 || gamepad_button_a() == 1 || gamepad_button_start() == 1 )
    {
        if( mode_unlocked( chosen_mode ) )
        {
            game_mode = chosen_mode;
            new_game();
        }
        else sfx( SND_SHIELD, 0.3 );
    }
    else if( gamepad_button_y() == 1 ) enter_title();
}

// -----------------------------------------------------------------------------
//   OPTIONS SCREEN
// -----------------------------------------------------------------------------

#define OPT_MUSIC    0
#define OPT_SFX      1
#define OPT_DIFF     2
#define OPT_SHAKE    3
#define OPT_FLASH    4
#define OPT_DISPLAY  5
#define OPT_RESET    6
#define OPT_BACK     7
#define OPT_ROWS     8

int opt_cursor = 0;
bool opt_confirm_reset = false;
int opt_note_timer = 0;

void enter_options()
{
    opt_cursor = 0;
    opt_confirm_reset = false;
    opt_note_timer = 0;
    state = STATE_OPTIONS;
    state_timer = 0;
}

void leave_options()
{
    refresh_card();
    if( card_state == CARD_OURS || card_state == CARD_EMPTY ) save_all();

    if( options_from_pause )
    {
        options_from_pause = false;
        state = STATE_PLAYING;
        state_timer = 0;
        return;
    }
    enter_title();
}

void reset_high_scores()
{
    init_table();
    load_builtin_demo();
    high_score = hs_score[ 0 ];
    refresh_card();
    if( card_state == CARD_OURS || card_state == CARD_EMPTY ) save_all();
    opt_note_timer = 150;
    sfx( SND_START, 0.6 );
}

void update_options()
{
    int change = 0, k;

    if( opt_note_timer > 0 ) opt_note_timer--;

    if( pressed_or_repeat( gamepad_up() ) )
    {
        opt_cursor = ( opt_cursor + OPT_ROWS - 1 ) % OPT_ROWS;
        opt_confirm_reset = false;
        sfx( SND_BEEP, 0.35 );
    }
    if( pressed_or_repeat( gamepad_down() ) )
    {
        opt_cursor = ( opt_cursor + 1 ) % OPT_ROWS;
        opt_confirm_reset = false;
        sfx( SND_BEEP, 0.35 );
    }

    if( pressed_or_repeat( gamepad_left() ) ) change = -1;
    if( pressed_or_repeat( gamepad_right() ) ) change = 1;

    if( change != 0 )
    {
        if( opt_cursor == OPT_DISPLAY )
        {
            // step through the displays unlocked so far
            k = ( opt_display + DISPLAYS + change ) % DISPLAYS;
            while( !display_unlocked( k ) ) k = ( k + DISPLAYS + change ) % DISPLAYS;
            if( k == opt_display ) sfx( SND_SHIELD, 0.3 );
            else sfx( SND_BEEP, 0.35 );
            opt_display = k;
        }
        else if( opt_cursor == OPT_MUSIC )
        {
            opt_music = max( 0, min( 10, opt_music + change ) );
            apply_music_volume();                  // hear the new level
        }
        else if( opt_cursor == OPT_SFX )
        {
            opt_sfx = max( 0, min( 10, opt_sfx + change ) );
            sfx( SND_LASER, 0.4 );                 // hear the new level
        }
        else if( opt_cursor == OPT_DIFF )
        {
            // shown in the order easy, normal, hard
            if( opt_difficulty == DIFF_EASY && change > 0 ) opt_difficulty = DIFF_NORMAL;
            else if( opt_difficulty == DIFF_NORMAL && change > 0 ) opt_difficulty = DIFF_HARD;
            else if( opt_difficulty == DIFF_HARD && change < 0 ) opt_difficulty = DIFF_NORMAL;
            else if( opt_difficulty == DIFF_NORMAL && change < 0 ) opt_difficulty = DIFF_EASY;
            sfx( SND_BEEP, 0.35 );
        }
        else if( opt_cursor == OPT_SHAKE )
        {
            opt_shake = !opt_shake;
            sfx( SND_BEEP, 0.35 );
            add_shake( 6 );
        }
        else if( opt_cursor == OPT_FLASH )
        {
            opt_flash = !opt_flash;
            sfx( SND_BEEP, 0.35 );
            add_flash( 0.5, 0xFF4040FF );
        }
    }

    if( gamepad_button_b() == 1 || gamepad_button_a() == 1 )
    {
        if( opt_cursor == OPT_RESET )
        {
            if( !opt_confirm_reset )
            {
                opt_confirm_reset = true;
                sfx( SND_SHIELD, 0.4 );
            }
            else
            {
                opt_confirm_reset = false;
                reset_high_scores();
            }
        }
        else if( opt_cursor == OPT_BACK )
        {
            leave_options();
            return;
        }
    }

    if( gamepad_button_start() == 1 || gamepad_button_y() == 1 ) leave_options();
}

// -----------------------------------------------------------------------------
//   PAUSE MENU
// -----------------------------------------------------------------------------

#define PAUSE_RESUME   0
#define PAUSE_OPTIONS  1
#define PAUSE_QUIT     2

int pause_cursor = 0;
bool pause_confirm_quit = false;

void pause_game()
{
    paused = true;
    pause_cursor = PAUSE_RESUME;
    pause_confirm_quit = false;
    pause_all_channels();
    sfx( SND_BEEP, 0.4 );
}

void resume_game()
{
    paused = false;
    resume_all_channels();
    sfx( SND_BEEP, 0.4 );
    input_lock = true;            // the button that chose Resume mustn't fire
}

void update_pause_menu()
{
    if( pressed_or_repeat( gamepad_up() ) )
    {
        pause_cursor = ( pause_cursor + 2 ) % 3;
        pause_confirm_quit = false;
        sfx( SND_BEEP, 0.35 );
    }
    if( pressed_or_repeat( gamepad_down() ) )
    {
        pause_cursor = ( pause_cursor + 1 ) % 3;
        pause_confirm_quit = false;
        sfx( SND_BEEP, 0.35 );
    }

    if( gamepad_button_start() == 1 )
    {
        resume_game();
        return;
    }

    if( gamepad_button_b() == 1 || gamepad_button_a() == 1 )
    {
        if( pause_cursor == PAUSE_RESUME ) resume_game();
        else if( pause_cursor == PAUSE_OPTIONS )
        {
            options_from_pause = true;
            enter_options();
        }
        else if( !pause_confirm_quit )
        {
            pause_confirm_quit = true;
            sfx( SND_SHIELD, 0.4 );
        }
        else
        {
            // abandon the game: not scored, never becomes the demo,
            // but any feats earned in it are kept
            paused = false;
            resume_all_channels();
            if( progress_dirty ) save_all();
            enter_title();
        }
    }
}

void update_aim()
{
    float ax = 0, ay = 0;
    if( in_bits & IN_LEFT ) ax = -1;
    if( in_bits & IN_RIGHT ) ax = 1;
    if( in_bits & IN_UP ) ay = -1;
    if( in_bits & IN_DOWN ) ay = 1;
    aim_vx += ( ax * 6.0 - aim_vx ) * 0.25;
    aim_vy += ( ay * 5.0 - aim_vy ) * 0.25;
    aim_x = clampf( aim_x + aim_vx, 40, 600 );
    aim_y = clampf( aim_y + aim_vy, 40, 320 );

    // in space, holding the crosshair off-centre slowly turns the view
    turn_x = 0;  turn_y = 0;
    if( phase == PH_SPACE )
    {
        turn_x = ( aim_x - SCREEN_CX ) / 280.0 * 0.0035;
        turn_y = ( aim_y - SCREEN_CY ) / 140.0 * 0.0025;
    }
}

void update_playing()
{
    if( !demo_mode )
    {
        if( paused )
        {
            update_pause_menu();
            if( paused || state != STATE_PLAYING ) return;
        }
        else if( gamepad_button_start() == 1 )
        {
            pause_game();
            return;
        }
    }

    // this logic frame's input: replayed, or read live and recorded
    if( demo_mode )
    {
        in_bits = rec_get( demo_data, demo_pos );
        demo_pos++;
    }
    else
    {
        in_bits = read_gamepad_bits();
        rec_add( in_bits );
    }
    game_frame++;
    phase_timer++;
    if( banner_timer > 0 ) banner_timer--;
    if( invuln > 0 ) invuln--;
    if( laser_timer > 0 ) laser_timer--;
    if( fire_cooldown > 0 ) fire_cooldown--;
    if( hit_flash > 0 ) hit_flash--;

    if( game_mode == MODE_TIME && !p_dead && phase != PH_BOOM && phase != PH_CORE )
    {
        time_left--;
        if( time_left <= 0 )
        {
            time_left = 0;
            p_dead = true;
            dead_timer = 0;
            sfx( SND_GAME_OVER, 0.5 );
        }
    }

    if( p_dead )
    {
        dead_timer++;
        turn_x = 0;  turn_y = 0;
        scroll = 0;
        if( dead_timer > 160 )
        {
            if( demo_mode ) enter_title();
            else finish_game();
        }
        return;
    }

    if( phase != PH_BOOM ) update_aim();
    else { turn_x = 0;  turn_y = 0; }

    if( phase == PH_SPACE ) update_space();
    else if( phase == PH_HULL ) update_hull();
    else if( phase == PH_TRENCH ) update_trench_phase();
    else if( phase == PH_CORE ) update_core();
    else update_boom();

    update_fireballs();

    if( state != STATE_PLAYING && state != STATE_DEMO ) return;
    if( phase != PH_BOOM && ( in_bits & IN_FIRE ) && fire_cooldown == 0 && !p_dead )
    {
        fire_lasers();
        fire_cooldown = 9;
    }
}

// -----------------------------------------------------------------------------
//   HUD AND SCREENS
// -----------------------------------------------------------------------------

#define HUD_CORE 0xFF60FFFF
#define HUD_DIM  0xFF40A0A0
#define MENU_SEL  0xFFFFFF60
#define MENU_SEL_GLOW 0xFF606020
#define WHITE_CORE 0xFFFFFFFF
#define WHITE_GLOW 0xFF505050

void draw_pause_menu();

float number_width( int value, float size )
{
    int digits = 1;
    while( value >= 10 ) { value /= 10; digits++; }
    return ( digits * 1.6 - 0.6 ) * size;
}

void draw_crosshair()
{
    float x = aim_x, y = aim_y, g = 0;
    find_target();
    vec_thickness( 1.8 );
    vec_glow_size( 10 );
    if( target_type != TG_NONE )
    {
        vec_color( 0xFF5050FF, 0xFF151580 );
        g = 3;
    }
    else vec_color( 0xFFFFFF60, 0xFF606020 );
    vec_line( x - 18 + g, y, x - 7 + g, y );
    vec_line( x + 7 - g, y, x + 18 - g, y );
    vec_line( x, y - 14 + g, x, y - 6 + g );
    vec_line( x, y + 6 - g, x, y + 14 - g );
    vec_line( x - 4, y - 4, x + 4, y - 4 );
    vec_line( x + 4, y - 4, x + 4, y + 4 );
    vec_line( x + 4, y + 4, x - 4, y + 4 );
    vec_line( x - 4, y + 4, x - 4, y - 4 );

    if( hit_flash > 0 )
    {
        vec_color( 0xFFFFFFFF, 0xFF606060 );
        vec_circle( hit_flash_x, hit_flash_y, 14 - hit_flash, 8 );
    }
}

// a feat just achieved, and what it unlocks
void draw_toast()
{
    float f;
    if( toast_feat < 0 || toast_timer <= 0 || paused ) return;
    f = fmin( 1.0, fmin( ( 220 - toast_timer ) / 15.0, toast_timer / 30.0 ) );
    vec_intensity( f );
    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( 0xFF80D0FF, 0 );
    vec_text_centered( "PILOT RECORD", 320, 234, 4.5 );
    vec_thickness( 2.2 );
    vec_glow_size( 12 );
    vec_color( MENU_SEL, MENU_SEL_GLOW );
    vec_text_centered( feat_title( toast_feat ), 320, 250, 8 );
    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( HUD_DIM, 0 );
    vec_text_centered( feat_reward( toast_feat ), 320, 274, 4.5 );
    vec_intensity( 1 );
}

void draw_hud()
{
    int i;
    float x;

    // the cockpit sways against your aim and rattles when hit
    ck_ox = -aim_vx * 1.2 + fx_range( -shake, shake ) * 0.5;
    ck_oy = -aim_vy * 1.2 + fx_range( -shake, shake ) * 0.5;
    vec_offset( ck_ox, ck_oy );
    draw_cracks();
    draw_cockpit();
    vec_offset( 0, 0 );

    vec_thickness( 1.8 );
    vec_glow_size( 0 );
    vec_color( HUD_DIM, 0 );
    vec_text( "SCORE", 16, 12, 5 );
    vec_text( "HIGH", 568, 12, 5 );
    vec_text( "WAVE", 16, 52, 4.5 );
    vec_color( HUD_CORE, 0 );
    vec_number( score, 16, 26, 7 );
    vec_number( high_score, 624 - number_width( high_score, 7 ), 26, 7 );
    vec_number( wave, 16 + vec_text_width( "WAVE ", 4.5 ), 52, 4.5 );
    if( game_mode != MODE_STANDARD )
    {
        vec_color( HUD_DIM, 0 );
        vec_text( mode_title( game_mode ), 16, 66, 3.8 );
    }

    if( game_mode == MODE_TIME )
    {
        vec_color( HUD_DIM, 0 );
        vec_text_centered( "TIME", 320, 326, 4.5 );
        vec_color( HUD_CORE, 0 );
        if( time_left < 20 * 60 && ( anim / 10 ) % 2 == 0 ) vec_color( 0xFF5050FF, 0 );
        i = ( time_left + 59 ) / 60;
        vec_number( i, 320 - number_width( i, 7 ) / 2, 338, 7 );
    }

    // shields: six cells along the bottom
    vec_color( HUD_DIM, 0 );
    if( game_mode != MODE_TIME ) vec_text_centered( "SHIELDS", 320, 326, 4.5 );
    for( i = 0; i < max_shields && game_mode != MODE_TIME; i++ )
    {
        x = 320 - max_shields * 17 + i * 34 + 4;
        vec_color( HUD_DIM, 0 );
        if( i < shields )
        {
            vec_color( HUD_CORE, 0 );
            if( shields <= 2 && ( anim / 10 ) % 2 == 0 ) vec_color( 0xFF5050FF, 0 );
            vec_line( x + 3, 344, x + 23, 344 );
            vec_line( x + 3, 347, x + 23, 347 );
        }
        vec_line( x, 340, x + 26, 340 );
        vec_line( x + 26, 340, x + 26, 351 );
        vec_line( x + 26, 351, x, 351 );
        vec_line( x, 351, x, 340 );
    }

    // the phase's progress, top centre
    vec_color( HUD_DIM, 0 );
    if( phase == PH_SPACE )
    {
        vec_text( "INTERCEPTORS", 250, 12, 4.5 );
        vec_number( max( 0, kill_quota - kills ), 250 + vec_text_width( "INTERCEPTORS ", 4.5 ), 12, 4.5 );
    }
    else if( phase == PH_HULL )
    {
        vec_text( "TOWERS", 276, 12, 4.5 );
        vec_number( max( 0, towers_quota - towers_spawned + count_towers() ), 276 + vec_text_width( "TOWERS ", 4.5 ), 12, 4.5 );
    }
    else if( phase == PH_TRENCH )
    {
        vec_text( "VENT", 286, 12, 4.5 );
        vec_number( max( 0, trench_len - trench_pos ), 286 + vec_text_width( "VENT ", 4.5 ), 12, 4.5 );
        if( trench_len - trench_pos < 200 && ( anim / 8 ) % 2 == 0 )
        {
            vec_color( 0xFF5050FF, 0 );
            vec_text_centered( "FIRE AT THE VENT", 320, 80, 6 );
        }
    }
    else if( phase == PH_CORE )
    {
        x = 320 - ( vec_text_width( "CELLS   OF 24", 4.5 ) + number_width( cells_hit, 4.5 ) ) / 2;
        vec_text( "CELLS", x, 12, 4.5 );
        x += vec_text_width( "CELLS ", 4.5 );
        vec_number( cells_hit, x, 12, 4.5 );
        vec_text( "OF 24", x + number_width( cells_hit, 4.5 ) + 1.6 * 4.5, 12, 4.5 );
    }

    // the phase banner
    if( banner_timer > 0 && !paused )
    {
        vec_intensity( fmin( 1.0, banner_timer / 30.0 ) );
        vec_thickness( 2.6 );
        vec_glow_size( 14 );
        vec_color( HUD_CORE, 0xFF205050 );
        vec_text_centered( banner_text, 320, 112, 11 );
        vec_thickness( 1.6 );
        vec_glow_size( 0 );
        vec_color( HUD_DIM, 0 );
        vec_text_centered( banner_sub, 320, 146, 5 );
        vec_intensity( 1 );
    }

    if( !p_dead && phase != PH_BOOM && !paused ) draw_crosshair();
    draw_lasers();

    if( p_dead )
    {
        vec_thickness( 2.6 );
        vec_glow_size( 14 );
        vec_color( 0xFF5050FF, 0xFF151580 );
        if( game_mode == MODE_TIME ) vec_text_centered( "TIME UP", 320, 150, 12 );
        else vec_text_centered( "SHIELDS GONE", 320, 150, 12 );
    }
    draw_toast();
    if( paused && state == STATE_PLAYING ) draw_pause_menu();
}

void draw_world()
{
    float e, t, a, r1, r2, wi = world_intensity;
    int k;
    set_view();
    vec_intensity( world_intensity );
    if( phase == PH_SPACE || phase == PH_BOOM || !in_game() )
    {
        if( phase == PH_BOOM && in_game() ) draw_stars( 9 );
        else if( phase == PH_SPACE && in_game() && phase_timer < 70 )
        {
            // dropping out of the jump to the next dreadnought
            e = 1 - phase_timer / 70.0;
            draw_stars( 3 + 70 * e * e );
        }
        else draw_stars( 3 );
        if( phase == PH_SPACE && in_game() ) draw_dreadnought_far();
        if( phase == PH_SPACE )
        {
            draw_bombers();
            draw_fighters();
            draw_mines();
        }
    }
    else
    {
        // diving in: the nose pitches down onto the hull or into the trench, then levels out
        if( in_game() && phase_timer < 70 )
        {
            e = 1 - phase_timer / 70.0;
            world_intensity = wi * e;
            draw_stars( 3 );
            world_intensity = wi;
            view_dy = -230 * e * e * e;
            set_view();
        }
        if( phase == PH_HULL ) draw_hull();
        else if( phase == PH_CORE ) draw_core();
        else draw_trench();
    }

    if( phase == PH_BOOM && in_game() )
    {
        if( boom_timer < 50 && !from_core )
        {
            // pulling up and out of the trench
            world_intensity = wi * ( 1 - boom_timer / 50.0 );
            view_dy = boom_timer * boom_timer * 0.09;
            set_view();
            draw_trench();
            view_dy = 0;
            set_view();
            world_intensity = wi;
        }
        if( boom_timer >= 30 && boom_timer < BOOM_BLAST )
        {
            vec_color( class_core(), vec_scale_rgb( class_core(), 0.3 ) );
            draw_dn_shape( 320, boom_dn_y(), boom_dn_scale(), 0.75 + 0.25 * sin( boom_timer * 0.9 ) );
        }
    }

    draw_fireballs();
    draw_debris();
    view_dy = 0;
    set_view();

    if( phase == PH_BOOM && in_game() && boom_timer >= BOOM_BLAST )
    {
        t = boom_timer - BOOM_BLAST;
        if( t >= 3 ) draw_chunks();       // the first instants are lost in the flash

        // rings of light, and a flat shockwave seen edge-on
        vec_thickness( 2.5 );
        vec_glow_size( 12 );
        vec_color( 0xFFFFFFFF, 0xFF406080 );
        vec_intensity( wi * fmax( 0, 1 - t / 175.0 ) );
        vec_circle( 320, 175, t * 3.4, 22 );
        vec_circle( 320, 175, t * 2.0, 14 );
        vec_color( class_core(), vec_scale_rgb( class_core(), 0.4 ) );
        r1 = 30 + t * 5.5;
        vec_glow_size( 6 );
        for( k = 0; k < 16; k++ )
        {
            a = k * 0.3926991;
            vec_line( 320 + cos( a ) * r1, 175 + sin( a ) * r1 * 0.16,
                      320 + cos( a + 0.3926991 ) * r1, 175 + sin( a + 0.3926991 ) * r1 * 0.16 );
        }

        // the first instant: rays bursting out of the core
        if( t < 50 )
        {
            vec_thickness( 2 );
            vec_glow_size( 0 );
            vec_color( 0xFFFFFFFF, 0 );
            vec_intensity( wi * ( 1 - t / 50.0 ) );
            for( k = 0; k < 12; k++ )
            {
                a = k * 0.5235988 + 0.2;
                r1 = 8 + t * 5;
                r2 = r1 + 60 * ( 1 - t / 50.0 );
                vec_line( 320 + cos( a ) * r1, 175 + sin( a ) * r1, 320 + cos( a ) * r2, 175 + sin( a ) * r2 );
            }
        }
        vec_intensity( wi );
    }
}

void draw_card_status()
{
    vec_thickness( 1.2 );
    vec_glow_size( 0 );
    vec_color( 0xFF40A0A0, 0 );
    if( card_state == CARD_OURS )       vec_text_centered( "HIGH SCORES SAVED ON MEMORY CARD", 320, 342, 4.2 );
    else if( card_state == CARD_EMPTY ) vec_text_centered( "MEMORY CARD READY", 320, 342, 4.2 );
    else if( card_state == CARD_OTHER ) vec_text_centered( "MEMORY CARD HAS ANOTHER GAME - NOT SAVING", 320, 342, 4.2 );
    else                                vec_text_centered( "NO MEMORY CARD - HIGH SCORES WILL NOT BE SAVED", 320, 342, 4.2 );
}

void draw_save_message( float y )
{
    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( HUD_CORE, 0 );
    if( save_message == SAVE_MSG_SAVED )        vec_text_centered( "SAVED TO MEMORY CARD", 320, y, 5 );
    else if( save_message == SAVE_MSG_NO_CARD ) vec_text_centered( "NO MEMORY CARD - NOT SAVED", 320, y, 5 );
    else                                        vec_text_centered( "CARD HAS ANOTHER GAME - NOT SAVED", 320, y, 5 );
}

void draw_scores_page()
{
    int i, k;
    float y, t;
    int[ 4 ] name;
    int[ 2 ] rank_text;

    t = fmin( 1.0, state_timer / 30.0 );
    vec_intensity( t );
    vec_thickness( 3 );
    vec_glow_size( 22 );
    vec_color( 0xFF60FFFF, 0xFF206060 );
    vec_text_centered( "HIGH SCORES", 320, 34, 15 );

    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( HUD_DIM, 0 );
    vec_dashed_line( 160, 82, 480, 82, 8, 6 );

    rank_text[ 1 ] = 0;
    for( i = 0; i < HS_COUNT; i++ )
    {
        y = 104 + i * 40;
        vec_intensity( t * fmin( 1.0, fmax( 0.0, ( state_timer - i * 8 ) / 20.0 ) ) );
        vec_thickness( 2 );
        vec_glow_size( 12 );
        if( i == highlight_rank )
        {
            if( ( frame / 10 ) % 2 ) vec_color( MENU_SEL, MENU_SEL_GLOW );
            else vec_color( WHITE_CORE, WHITE_GLOW );
        }
        else if( i == 0 ) vec_color( 0xFF60FFFF, 0xFF206060 );
        else vec_color( 0xFF40D0D0, 0xFF104040 );

        rank_text[ 0 ] = '1' + i;
        vec_text( rank_text, 170, y, 9 );
        unpack_name( hs_name[ i ], name );
        vec_text( name, 220, y, 9 );
        vec_number( hs_score[ i ], 470 - number_width( hs_score[ i ], 9 ), y, 9 );

        // a small tag for scores set on easy or hard
        k = ( hs_name[ i ] >> 24 ) & 3;
        if( k != DIFF_NORMAL )
        {
            vec_thickness( 1.5 );
            vec_glow_size( 0 );
            if( k == DIFF_EASY ) { vec_color( 0xFF90E090, 0 ); vec_char( 'E', 484, y + 6, 4 ); }
            else                 { vec_color( 0xFF5050FF, 0 ); vec_char( 'H', 484, y + 6, 4 ); }
        }
    }

    vec_intensity( t );
    if( save_message != SAVE_MSG_NONE && highlight_rank >= 0 ) draw_save_message( 312 );
    else if( ( frame / 30 ) % 2 == 0 )
    {
        vec_thickness( 2 );
        vec_glow_size( 0 );
        vec_color( WHITE_CORE, 0 );
        vec_text_centered( "PRESS START", 320, 308, 8 );
    }
    draw_card_status();
    vec_intensity( 1 );
}

void draw_entry()
{
    int i;
    float x, w, t;
    int[ 2 ] letter;

    t = fmin( 1.0, state_timer / 25.0 );
    vec_intensity( t );
    vec_thickness( 3 );
    vec_glow_size( 22 );
    vec_color( 0xFF60FFFF, 0xFF206060 );
    if( entry_mode != MODE_STANDARD ) vec_text_centered( mode_title( entry_mode ), 320, 30, 14 );
    else if( entry_rank == 0 ) vec_text_centered( "NEW HIGH SCORE", 320, 30, 14 );
    else vec_text_centered( "TOP 5 SCORE", 320, 30, 14 );

    vec_thickness( 2.5 );
    vec_glow_size( 16 );
    vec_color( WHITE_CORE, WHITE_GLOW );
    w = number_width( score, 12 );
    vec_number( score, 320 - w / 2, 84, 12 );

    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( HUD_DIM, 0 );
    if( entry_mode != MODE_STANDARD ) vec_text_centered( "NEW BEST - ENTER YOUR INITIALS", 320, 136, 6 );
    else vec_text_centered( "ENTER YOUR INITIALS", 320, 136, 6 );

    letter[ 1 ] = 0;
    for( i = 0; i < 3; i++ )
    {
        x = 320 - 90 + i * 70;
        letter[ 0 ] = entry_charset[ entry_index[ i ] ];
        if( i == entry_cursor )
        {
            vec_thickness( 3.5 );
            vec_glow_size( 24 );
            vec_color( MENU_SEL, MENU_SEL_GLOW );
            vec_text( letter, x, 170 + 3 * sin( frame * 0.2 ), 20 );
            if( ( frame / 15 ) % 2 == 0 ) vec_line( x - 4, 226, x + 24, 226 );
        }
        else
        {
            vec_thickness( 3 );
            vec_glow_size( 16 );
            vec_color( 0xFF60FFFF, 0xFF206060 );
            vec_text( letter, x, 170, 20 );
            vec_line( x - 4, 226, x + 24, 226 );
        }
        if( letter[ 0 ] == ' ' )
        {
            vec_thickness( 1.5 );
            vec_dashed_line( x, 210, x + 20, 210, 3, 3 );
        }
    }

    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( HUD_DIM, 0 );
    vec_text_centered( "UP DOWN  CHANGE     LEFT RIGHT  MOVE", 320, 268, 5 );
    vec_text_centered( "B  NEXT     START  DONE", 320, 290, 5 );
    vec_intensity( 1 );
}

void draw_demo_overlay()
{
    if( ( frame / 40 ) % 2 == 0 )
    {
        vec_thickness( 2.5 );
        vec_glow_size( 0 );
        vec_color( 0xFF60FFFF, 0 );
        vec_text_centered( "DEMO", 320, 252, 10 );
    }
    vec_thickness( 2 );
    vec_glow_size( 0 );
    vec_color( WHITE_CORE, 0 );
    vec_text_centered( "PRESS START", 320, 296, 7 );
}

void draw_value_arrows( float x_left, float x_right, float y )
{
    vec_line( x_left, y + 6, x_left + 6, y );
    vec_line( x_left, y + 6, x_left + 6, y + 12 );
    vec_line( x_right, y + 6, x_right - 6, y );
    vec_line( x_right, y + 6, x_right - 6, y + 12 );
}

void draw_options()
{
    int i, k;
    float y, t, x;
    bool selected;
    int* value;

    t = fmin( 1.0, state_timer / 25.0 );
    vec_intensity( t );
    vec_thickness( 3 );
    vec_glow_size( 22 );
    vec_color( 0xFF60FFFF, 0xFF206060 );
    vec_text_centered( "OPTIONS", 320, 30, 15 );

    for( i = 0; i < OPT_ROWS; i++ )
    {
        y = 88 + i * 27;
        if( i == OPT_RESET ) y += 10;
        if( i == OPT_BACK ) y += 14;
        selected = ( i == opt_cursor );

        vec_thickness( 1.8 );
        vec_glow_size( 0 );
        if( selected ) vec_color( MENU_SEL, 0 );
        else vec_color( HUD_DIM, 0 );

        if( i == OPT_MUSIC ) vec_text( "MUSIC VOLUME", 110, y, 6 );
        else if( i == OPT_SFX ) vec_text( "EFFECTS VOLUME", 110, y, 6 );
        else if( i == OPT_DIFF ) vec_text( "DIFFICULTY", 110, y, 6 );
        else if( i == OPT_SHAKE ) vec_text( "SCREEN SHAKE", 110, y, 6 );
        else if( i == OPT_FLASH ) vec_text( "SCREEN FLASHES", 110, y, 6 );
        else if( i == OPT_DISPLAY ) vec_text( "DISPLAY", 110, y, 6 );
        else if( i == OPT_RESET ) vec_text( "RESET HIGH SCORES", 110, y, 6 );
        else vec_text( "BACK", 110, y, 6 );

        if( selected )
        {
            x = 92 + 3 * sin( frame * 0.2 );
            vec_line( x, y + 1, x + 8, y + 6 );
            vec_line( x, y + 11, x + 8, y + 6 );
        }

        if( i == OPT_SFX || i == OPT_MUSIC )
        {
            vec_thickness( 6 );
            vec_glow_size( 0 );
            for( k = 0; k < 10; k++ )
            {
                if( ( i == OPT_SFX && k < opt_sfx ) || ( i == OPT_MUSIC && k < opt_music ) ) vec_color( 0xFFFFFF60, 0 );
                else vec_color( 0xFF505020, 0 );
                vec_line( 380 + k * 14, y + 6, 389 + k * 14, y + 6 );
            }
        }
        else if( i == OPT_DIFF || i == OPT_SHAKE || i == OPT_FLASH || i == OPT_DISPLAY )
        {
            if( i == OPT_DISPLAY ) value = display_name( opt_display );
            else if( i == OPT_DIFF )
            {
                value = "NORMAL";
                if( opt_difficulty == DIFF_EASY ) value = "EASY";
                if( opt_difficulty == DIFF_HARD ) value = "HARD";
            }
            else
            {
                value = "OFF";
                if( i == OPT_SHAKE && opt_shake ) value = "ON";
                if( i == OPT_FLASH && opt_flash ) value = "ON";
            }
            vec_thickness( 2 );
            vec_glow_size( 0 );
            if( selected ) vec_color( WHITE_CORE, 0 );
            else vec_color( 0xFF60D0D0, 0 );
            vec_text_centered( value, 449, y, 6 );
            if( selected ) draw_value_arrows( 384, 514, y );
        }
        else if( i == OPT_RESET && opt_confirm_reset && ( frame / 10 ) % 2 == 0 )
        {
            vec_thickness( 2 );
            vec_glow_size( 0 );
            vec_color( 0xFF5050FF, 0 );
            vec_text_centered( "PRESS B AGAIN", 449, y, 5 );
        }
    }

    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( 0xFF60D0D0, 0 );
    if( opt_cursor == OPT_DIFF )
    {
        if( options_from_pause )               vec_text_centered( "TAKES EFFECT IN THE NEXT GAME", 320, 316, 4.5 );
        else if( opt_difficulty == DIFF_EASY ) vec_text_centered( "8 SHIELDS - SLOWER FIREBALLS - LESS ENEMY FIRE", 320, 316, 4.5 );
        else if( opt_difficulty == DIFF_HARD ) vec_text_centered( "4 SHIELDS - FASTER FIREBALLS - MORE ENEMY FIRE", 320, 316, 4.5 );
        else                                   vec_text_centered( "6 SHIELDS - THE STANDARD GAME", 320, 316, 4.5 );
    }
    else if( opt_note_timer > 0 ) vec_text_centered( "HIGH SCORES RESET", 320, 316, 5 );

    vec_color( HUD_DIM, 0 );
    vec_thickness( 1.2 );
    vec_text_centered( "UP DOWN SELECT   LEFT RIGHT CHANGE   START DONE", 320, 340, 4.2 );
    vec_intensity( 1 );
}

void draw_pause_menu()
{
    int i;
    float y, x;
    int* label;

    vec_intensity( 1 );
    vec_thickness( 3 );
    vec_glow_size( 18 );
    vec_color( WHITE_CORE, WHITE_GLOW );
    vec_text_centered( "PAUSED", 320, 130, 13 );

    for( i = 0; i <= PAUSE_QUIT; i++ )
    {
        y = 186 + i * 28;
        label = "RESUME";
        if( i == PAUSE_OPTIONS ) label = "OPTIONS";
        if( i == PAUSE_QUIT ) label = "QUIT TO TITLE";
        if( i == PAUSE_QUIT && pause_confirm_quit ) label = "PRESS B AGAIN TO QUIT";

        vec_thickness( 2 );
        vec_glow_size( 0 );
        if( i == pause_cursor )
        {
            if( i == PAUSE_QUIT && pause_confirm_quit ) vec_color( 0xFF5050FF, 0 );
            else vec_color( MENU_SEL, 0 );
            x = 320 - vec_text_width( label, 7 ) / 2 - 20 + 3 * sin( frame * 0.2 );
            vec_line( x, y + 1, x + 9, y + 7 );
            vec_line( x, y + 13, x + 9, y + 7 );
        }
        else vec_color( HUD_DIM, 0 );
        vec_text_centered( label, 320, y, 7 );
    }

    vec_thickness( 1.2 );
    vec_glow_size( 0 );
    vec_color( HUD_DIM, 0 );
    vec_text_centered( "UP DOWN SELECT   B CHOOSE   START RESUME", 320, 280, 4.2 );
}

// the pilot record: the feats, with a spotlight that steps down the list
// telling how each is earned and what it unlocks (text is the costly thing
// to draw, so the page keeps it short)
void draw_record_page()
{
    int i, k;
    float y, t;
    bool got, shown;

    t = fmin( 1.0, state_timer / 30.0 );
    vec_intensity( t );
    vec_thickness( 3 );
    vec_glow_size( 22 );
    vec_color( 0xFF60FFFF, 0xFF206060 );
    vec_text_centered( "PILOT RECORD", 320, 22, 13 );

    k = ( state_timer / 60 ) % FEATS;
    vec_glow_size( 0 );
    for( i = 0; i < FEATS; i++ )
    {
        y = 74 + i * 25;
        got = has_feat( i );
        shown = got || !feat_hidden( i );
        vec_thickness( 1.8 );
        if( got )
        {
            vec_color( 0xFF60FF60, 0 );
            vec_line( 150, y + 6, 155, y + 11 );
            vec_line( 155, y + 11, 164, y );
        }
        else
        {
            vec_color( 0xFF406868, 0 );
            vec_line( 152, y + 6, 162, y + 6 );
        }
        if( i == k ) vec_color( MENU_SEL, 0 );
        else if( got ) vec_color( HUD_DIM, 0 );
        else vec_color( 0xFF406868, 0 );
        if( shown ) vec_text( feat_title( i ), 180, y, 5.5 );
        else vec_text( "? ? ? ? ?", 180, y, 5.5 );
    }

    // the spotlit feat
    got = has_feat( k );
    shown = got || !feat_hidden( k );
    vec_thickness( 1.4 );
    vec_color( HUD_DIM, 0 );
    if( shown ) vec_text_centered( feat_about( k ), 320, 284, 4.4 );
    else vec_text_centered( "A SECRET", 320, 284, 4.4 );
    if( shown )
    {
        if( got ) vec_color( 0xFF80D0FF, 0 );
        else vec_color( 0xFF406868, 0 );
        vec_text_centered( feat_reward( k ), 320, 304, 4.4 );
    }
    vec_intensity( 1 );
}

void draw_modes()
{
    int i, best;
    float y, t, x;
    bool selected, open;
    int[ 4 ] name;

    t = fmin( 1.0, state_timer / 25.0 );
    vec_intensity( t );
    vec_thickness( 3 );
    vec_glow_size( 22 );
    vec_color( 0xFF60FFFF, 0xFF206060 );
    vec_text_centered( "SELECT MODE", 320, 30, 15 );

    for( i = 0; i < MODES; i++ )
    {
        y = 96 + i * 40;
        selected = ( i == chosen_mode );
        open = mode_unlocked( i );

        vec_thickness( 1.8 );
        vec_glow_size( 0 );
        if( selected ) vec_color( MENU_SEL, 0 );
        else if( open ) vec_color( HUD_DIM, 0 );
        else vec_color( 0xFF305050, 0 );
        if( open ) vec_text( mode_title( i ), 110, y, 7 );
        else vec_text( "? ? ? ? ?", 110, y, 7 );

        if( selected )
        {
            x = 92 + 3 * sin( frame * 0.2 );
            vec_line( x, y + 2, x + 8, y + 7 );
            vec_line( x, y + 12, x + 8, y + 7 );
        }

        // the best so far: the high score table's top, or the mode's own
        best = mode_best[ i ];
        unpack_name( mode_name[ i ], name );
        if( i == MODE_STANDARD )
        {
            best = hs_score[ 0 ];
            unpack_name( hs_name[ 0 ], name );
        }
        if( open && best > 0 )
        {
            vec_thickness( 1.6 );
            vec_color( 0xFF80D080, 0 );
            vec_number( best, 500 - number_width( best, 6 ), y + 1, 6 );
            vec_text( name, 516, y + 1, 6 );
        }
    }

    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( 0xFF80D0FF, 0 );
    if( mode_unlocked( chosen_mode ) ) vec_text_centered( mode_about( chosen_mode ), 320, 270, 4.6 );
    else vec_text_centered( mode_hint( chosen_mode ), 320, 270, 4.6 );

    vec_color( HUD_DIM, 0 );
    vec_thickness( 1.2 );
    vec_text_centered( "UP DOWN SELECT   B START   Y BACK", 320, 330, 4.6 );
    vec_intensity( 1 );
}

void draw_title()
{
    vec_thickness( 4 );
    vec_glow_size( 26 );
    vec_color( 0xFF60FFFF, 0xFF206060 );
    vec_text_centered( "LANCER", 320, 70, 24 );
    vec_thickness( 1.6 );
    vec_glow_size( 0 );
    vec_color( HUD_DIM, 0 );
    vec_text_centered( "STRIKE AT THE DREADNOUGHT", 320, 130, 6 );

    if( ( frame / 30 ) % 2 == 0 )
    {
        vec_thickness( 2 );
        vec_glow_size( 14 );
        vec_color( 0xFFFFFFFF, 0xFF505050 );
        vec_text_centered( "PRESS START", 320, 230, 9 );
    }
    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( HUD_DIM, 0 );
    vec_thickness( 1.5 );
    vec_glow_size( 0 );
    vec_color( MENU_SEL, 0 );
    vec_text_centered( "Y  OPTIONS", 320, 262, 6 );
    vec_color( HUD_DIM, 0 );
    vec_text_centered( "D-PAD AIM AND STEER   B FIRE   START PAUSE", 320, 284, 4.6 );
    vec_text( "HIGH SCORE", 222, 308, 6 );
    vec_number( high_score, 222 + vec_text_width( "HIGH SCORE ", 6 ), 308, 6 );
    draw_card_status();
    vec_thickness( 1 );
    vec_color( 0xFF608060, 0 );
    vec_text( LANCER_VERSION, 600, 330, 3.5 );
}

void draw_game_over()
{
    float x;
    vec_intensity( fmin( 1.0, state_timer / 30.0 ) );
    vec_thickness( 3 );
    vec_glow_size( 20 );
    vec_color( 0xFF5050FF, 0xFF151580 );
    vec_text_centered( "GAME OVER", 320, 120, 18 );
    vec_thickness( 2 );
    vec_glow_size( 0 );
    vec_color( HUD_CORE, 0 );
    x = 320 - ( vec_text_width( "SCORE ", 7 ) + number_width( score, 7 ) ) / 2;
    vec_text( "SCORE", x, 180, 7 );
    vec_number( score, x + vec_text_width( "SCORE ", 7 ), 180, 7 );
    x = 320 - ( vec_text_width( "WAVE ", 6 ) + number_width( wave, 6 ) ) / 2;
    vec_text( "WAVE", x, 206, 6 );
    vec_number( wave, x + vec_text_width( "WAVE ", 6 ), 206, 6 );
    if( state_timer > 90 && ( frame / 30 ) % 2 == 0 )
    {
        vec_color( 0xFFFFFFFF, 0 );
        vec_text_centered( "PRESS START", 320, 250, 8 );
    }
    if( save_message != SAVE_MSG_NONE ) draw_save_message( 290 );
    vec_intensity( 1 );
}

// -----------------------------------------------------------------------------
//   MUSIC DIRECTOR: a theme for each phase, the fanfare when a dreadnought
//   goes up, the sting at the end (sound only: replays are unaffected)
// -----------------------------------------------------------------------------

#define GAME_MUSIC_VOLUME  0.4

void update_music()
{
    int current = music_current();
    int want;
    float volume;

    // title pages, attract demo, options from the title
    if( state == STATE_TITLE || state == STATE_DEMO || state == STATE_MODES || ( state == STATE_OPTIONS && !options_from_pause ) )
    {
        if( current != MUSIC_TITLE ) play_track( MUSIC_TITLE, 0.45 );
        return;
    }

    // after a game: the sting, then the title theme softly
    if( state == STATE_ENTRY || state == STATE_GAMEOVER )
    {
        if( current == -1 ) play_track( MUSIC_TITLE, 0.3 );
        return;
    }

    // options from the pause menu: keep the music, quietly
    if( state == STATE_OPTIONS )
    {
        music_base += ( GAME_MUSIC_VOLUME * 0.35 - music_base ) * 0.1;
        apply_music_volume();
        return;
    }
    if( state != STATE_PLAYING ) return;

    // the last shield gone: silence until the sting
    if( p_dead )
    {
        if( current != -1 ) music_stop();
        return;
    }

    if( phase == PH_BOOM )
    {
        // silence while it burns, the fanfare at the blast
        if( boom_timer < BOOM_BLAST )
        {
            if( current != -1 ) music_stop();
        }
        else if( current != MUSIC_VICTORY && boom_timer < BOOM_BLAST + 5 ) play_track( MUSIC_VICTORY, 0.5 );
        return;
    }

    // the fanfare plays out over the start of the next wave
    if( current == MUSIC_VICTORY ) return;

    want = MUSIC_SPACE;
    if( phase == PH_HULL ) want = MUSIC_HULL;
    if( phase == PH_TRENCH ) want = MUSIC_TRENCH;
    if( phase == PH_CORE ) want = MUSIC_CORE;
    if( current != want ) play_track( want, GAME_MUSIC_VOLUME );

    volume = GAME_MUSIC_VOLUME;
    if( paused ) volume *= 0.35;
    music_base += ( volume - music_base ) * 0.1;
    apply_music_volume();
}

// -----------------------------------------------------------------------------
//   MAIN
// -----------------------------------------------------------------------------

void main()
{
    srand( get_time() + 1 );
    vec_init( 0, 0 );
    set_global_volume( 0.6 );     // headroom: the sound chip does not clamp its mix
    select_gamepad( 0 );
    select_sound( SND_HUM );
    set_sound_loop( true );
    music_init( SND_INSTRUMENTS, CH_MUSIC );
    fx_state = 4711;
    init_stars();
    init_table();
    load_builtin_demo();
    refresh_card();
    high_score = hs_score[ 0 ];
    enter_title();

    while( true )
    {
        // --- update ---
        if( state == STATE_TITLE )
        {
            refresh_card();            // notices cards inserted on the title screen
            turn_x = 0.0006 * sin( frame * 0.004 );
            turn_y = 0;
            update_stars( 1.5 );
            if( gamepad_button_start() == 1 && state_timer > 20 ) title_start();
            else if( gamepad_button_y() == 1 && state_timer > 20 ) enter_options();

            // logo -> high scores -> demo -> logo ...
            else if( title_page == PAGE_LOGO && state_timer > 480 )
              enter_title_page( PAGE_SCORES );
            else if( title_page == PAGE_SCORES && state_timer > 360 )
            {
                highlight_rank = -1;
                enter_title_page( PAGE_RECORD );
            }
            else if( title_page == PAGE_RECORD && state_timer > 480 )
            {
                game_mode = MODE_STANDARD;
                if( demo_frames > 0 ) begin_game( demo_seed, true );
                else enter_title_page( PAGE_LOGO );
            }
        }
        else if( state == STATE_PLAYING ) update_playing();
        else if( state == STATE_DEMO )
        {
            if( gamepad_button_start() == 1 ) title_start();
            else
            {
                update_playing();
                if( state == STATE_DEMO && ( demo_pos >= demo_frames || demo_pos >= DEMO_MAX_PLAY ) )
                  enter_title();
            }
        }
        else if( state == STATE_ENTRY )
        {
            update_stars( 1.0 );
            update_entry();
        }
        else if( state == STATE_MODES )
        {
            update_stars( 1.0 );
            update_modes();
        }
        else if( state == STATE_OPTIONS )
        {
            if( !options_from_pause ) update_stars( 1.0 );
            update_options();
        }
        else
        {
            update_stars( 0.8 );
            if( state_timer > 90 && gamepad_button_start() == 1 ) new_game();
            else if( state_timer > 900 ) enter_title();
        }

        anim = frame;
        if( in_game() ) anim = game_frame;

        if( !paused )
        {
            update_debris();
            update_glass();
            if( in_game() ) update_toast();
            shake *= 0.85;
            flash *= 0.88;
        }
        update_hum( in_game() && state != STATE_OPTIONS && !p_dead && !paused, 0.8 + 0.12 * scroll );
        update_music();
        music_update();
        state_timer++;

        // --- draw ---
        vec_set_display( opt_display, frame * 0.03 );
        clear_screen( make_color_rgb( 0, 0, 6 ) );
        vec_offset( fx_range( -shake, shake ), fx_range( -shake, shake ) );

        world_intensity = 1.0;
        if( state == STATE_TITLE && title_page == PAGE_LOGO ) world_intensity = 0.7;
        else if( state == STATE_TITLE || state == STATE_ENTRY || state == STATE_MODES || ( state == STATE_OPTIONS && !options_from_pause ) ) world_intensity = 0.35;
        if( state == STATE_GAMEOVER ) world_intensity = 0.4;
        if( paused ) world_intensity = 0.3;
        if( in_game() && p_dead ) world_intensity = fmax( 0.15, 1.0 - dead_timer / 80.0 );
        draw_world();
        vec_offset( 0, 0 );
        vec_flush();                   // the world, then the HUD: neither can crowd the other out of the queue

        if( state == STATE_TITLE )
        {
            if( title_page == PAGE_LOGO ) draw_title();
            else if( title_page == PAGE_RECORD ) draw_record_page();
            else draw_scores_page();
        }
        else if( state == STATE_ENTRY ) draw_entry();
        else if( state == STATE_MODES ) draw_modes();
        else if( state == STATE_OPTIONS ) draw_options();
        else if( state == STATE_GAMEOVER ) draw_game_over();
        else
        {
            draw_hud();
            if( state == STATE_DEMO ) draw_demo_overlay();
        }

        vec_flush();
        if( flash > 0.02 )
          vec_fill_rect( 0, 0, screen_width, screen_height, vec_scale_alpha( vec_filter_color( flash_color ), flash * 0.45 ), blending_add );

        frame++;
        end_frame();
    }
}
