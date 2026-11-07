// Headless Vircon32 runner: drives the reference console logic and
// rasterizes its GPU quads in software, dumping selected frames as PPM.
#include "ConsoleLogic/V32Console.hpp"
#include "ConsoleLogic/ExternalInterfaces.hpp"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
using namespace V32;

static const int W = 640, H = 360, TS = 1024;
static float fb[ H ][ W ][ 3 ];
static std::vector<uint8_t> textures[ 257 ];   // index 0 = bios, 1.. = cartridge
static int curTex = 0, blendMode = 0x20;
static GPUColor mul = { 255, 255, 255, 255 };
static int quadsThisFrame = 0, clearsTotal = 0;

static std::vector<uint8_t>& Tex( int id ) { return textures[ id + 1 ]; }

static void cbClear( GPUColor c )
{
    for( int y = 0; y < H; y++ ) for( int x = 0; x < W; x++ )
    { fb[y][x][0] = c.R / 255.f; fb[y][x][1] = c.G / 255.f; fb[y][x][2] = c.B / 255.f; }
    clearsTotal++;
}

static void cbDraw( GPUQuad& q )
{
    quadsThisFrame++;
    std::vector<uint8_t>& t = Tex( curTex );
    if( t.empty() ) return;

    // quad is a parallelogram: v0 origin, v1 = +u edge, v2 = +v edge
    GPUPoint& a = q.Vertices[0]; GPUPoint& b = q.Vertices[1]; GPUPoint& c = q.Vertices[2];
    float ux = b.x - a.x, uy = b.y - a.y, vx = c.x - a.x, vy = c.y - a.y;
    float det = ux * vy - uy * vx;
    if( fabs( det ) < 1e-6 ) return;

    float minx = W, miny = H, maxx = 0, maxy = 0;
    for( int i = 0; i < 4; i++ )
    {
        minx = std::min( minx, q.Vertices[i].x ); maxx = std::max( maxx, q.Vertices[i].x );
        miny = std::min( miny, q.Vertices[i].y ); maxy = std::max( maxy, q.Vertices[i].y );
    }
    int x0 = std::max( 0, (int)floor( minx ) ), x1 = std::min( W - 1, (int)ceil( maxx ) );
    int y0 = std::max( 0, (int)floor( miny ) ), y1 = std::min( H - 1, (int)ceil( maxy ) );

    float mr = mul.R / 255.f, mg = mul.G / 255.f, mb = mul.B / 255.f, ma = mul.A / 255.f;

    for( int y = y0; y <= y1; y++ ) for( int x = x0; x <= x1; x++ )
    {
        float px = x + 0.5f - a.x, py = y + 0.5f - a.y;
        float u = ( px * vy - py * vx ) / det;
        float v = ( ux * py - uy * px ) / det;
        if( u < 0 || u >= 1 || v < 0 || v >= 1 ) continue;

        float tx = a.texture_x + u * ( b.texture_x - a.texture_x ) + v * ( c.texture_x - a.texture_x );
        float ty = a.texture_y + u * ( b.texture_y - a.texture_y ) + v * ( c.texture_y - a.texture_y );
        int ix = std::min( TS - 1, std::max( 0, (int)floor( tx * TS ) ) );
        int iy = std::min( TS - 1, std::max( 0, (int)floor( ty * TS ) ) );
        uint8_t* s = &t[ ( iy * TS + ix ) * 4 ];

        float sa = s[3] / 255.f * ma;
        float sr = s[0] / 255.f * mr, sg = s[1] / 255.f * mg, sb = s[2] / 255.f * mb;
        float* d = fb[y][x];

        if( blendMode == 0x21 )       { d[0] += sr * sa; d[1] += sg * sa; d[2] += sb * sa; }
        else if( blendMode == 0x22 )  { d[0] -= sr * sa; d[1] -= sg * sa; d[2] -= sb * sa; }
        else { d[0] = sr * sa + d[0] * ( 1 - sa ); d[1] = sg * sa + d[1] * ( 1 - sa ); d[2] = sb * sa + d[2] * ( 1 - sa ); }

        for( int k = 0; k < 3; k++ ) d[k] = std::min( 1.f, std::max( 0.f, d[k] ) );
    }
}

static void cbMul( GPUColor c ) { mul = c; }
static void cbBlend( int m ) { blendMode = m; }
static void cbSelect( int id ) { curTex = id; }
static void cbLoad( int id, void* px )
{
    Tex( id ).assign( (uint8_t*)px, (uint8_t*)px + TS * TS * 4 );
}
static void cbUnloadCart() { for( int i = 1; i < 257; i++ ) textures[i].clear(); }
static void cbUnloadBios() { textures[0].clear(); }
static void cbLog( const std::string& s ) { fprintf( stderr, "[log] %s\n", s.c_str() ); }
static void cbThrow( const std::string& s ) { throw std::runtime_error( s ); }

static void SavePPM( const std::string& path )
{
    FILE* f = fopen( path.c_str(), "wb" );
    fprintf( f, "P6\n%d %d\n255\n", W, H );
    for( int y = 0; y < H; y++ ) for( int x = 0; x < W; x++ )
      for( int k = 0; k < 3; k++ ) fputc( (int)( fb[y][x][k] * 255 + 0.5f ), f );
    fclose( f );
}

int main( int argc, char** argv )
{
    // usage: headless bios.v32 cart.v32 outdir total_frames step
    //   runs total_frames frames, saving every step-th frame as outdir/fNNNN.ppm
    // optional environment variables:
    //   INPUT_SCRIPT=file   lines "start end control": control held for frames [start, end)
    //                       (0 Left 1 Right 2 Up 3 Down 4 Start 5 A 6 B 7 X 8 Y 9 L 10 R)
    //   FRAME_FROM / FRAME_TO  only save frames in this range
    //   AUDIO_OUT=file      raw 16-bit stereo 44100 Hz audio from FRAME_FROM on
    //   MEMCARD=file        memory card file (created blank if missing, saved at exit)
    //   SKIPLOG=1           print frames where the program didn't finish a frame in time
    Callbacks::ClearScreen = cbClear;   Callbacks::DrawQuad = cbDraw;
    Callbacks::SetMultiplyColor = cbMul; Callbacks::SetBlendingMode = cbBlend;
    Callbacks::SelectTexture = cbSelect; Callbacks::LoadTexture = cbLoad;
    Callbacks::UnloadCartridgeTextures = cbUnloadCart; Callbacks::UnloadBiosTexture = cbUnloadBios;
    Callbacks::LogLine = cbLog; Callbacks::ThrowException = cbThrow;

    std::string outdir = argv[3];
    int total = atoi( argv[4] ), step = atoi( argv[5] );

    V32Console console;
    console.LoadBios( argv[1] );
    console.LoadCartridge( argv[2] );
    console.SetGamepadConnection( 0, true );
    console.SetCurrentDate( 2026, 303 );
    console.SetCurrentTime( 15, 0, 0 );
    // optional memory card file: created blank if it doesn't exist yet
    if( getenv( "MEMCARD" ) )
    {
        FILE* test = fopen( getenv( "MEMCARD" ), "rb" );
        if( test ) fclose( test );
        else console.CreateMemoryCard( getenv( "MEMCARD" ) );
        console.LoadMemoryCard( getenv( "MEMCARD" ) );
    }

    console.SetPower( true );

    // optional input script: lines of "start_frame end_frame control_index"
    // (control index follows V32::GamepadControls: 0 Left 1 Right 2 Up 3 Down
    //  4 Start 5 A 6 B 7 X 8 Y 9 L 10 R); control held for [start, end)
    std::vector<int> inStart, inEnd, inCtl;
    if( getenv( "INPUT_SCRIPT" ) )
    {
        FILE* sf = fopen( getenv( "INPUT_SCRIPT" ), "r" );
        int s, e, c;
        while( sf && fscanf( sf, "%d %d %d", &s, &e, &c ) == 3 )
        { inStart.push_back( s ); inEnd.push_back( e ); inCtl.push_back( c ); }
        if( sf ) fclose( sf );
    }

    // optional raw audio capture (16-bit stereo 44100 Hz) and first frame to save
    FILE* audioFile = getenv( "AUDIO_OUT" ) ? fopen( getenv( "AUDIO_OUT" ), "wb" ) : nullptr;
    int frameFrom = getenv( "FRAME_FROM" ) ? atoi( getenv( "FRAME_FROM" ) ) : 0;
    int frameTo = getenv( "FRAME_TO" ) ? atoi( getenv( "FRAME_TO" ) ) : 1 << 30;
    SPUOutputBuffer audioBuffer;
    long peakAudio = 0;

    int maxQuads = 0, lastClears = 0, skipped = 0;
    for( int f = 0; f < total; f++ )
    {
        for( int c = 0; c <= 10; c++ )
        {
            bool held = false;
            for( size_t k = 0; k < inCtl.size(); k++ )
              if( inCtl[k] == c && f >= inStart[k] && f < inEnd[k] ) held = true;
            console.SetGamepadControl( 0, (GamepadControls)c, held );
        }
        quadsThisFrame = 0;
        console.RunNextFrame();
        if( audioFile )
        {
            console.GetFrameSoundOutput( audioBuffer );
            if( f >= frameFrom )
              fwrite( audioBuffer.Samples, sizeof(SPUSample), Constants::SPUSamplesPerFrame, audioFile );
            for( int k = 0; k < Constants::SPUSamplesPerFrame; k++ )
              peakAudio = std::max( peakAudio, (long)abs( audioBuffer.Samples[k].LeftSample ) );
        }
        maxQuads = std::max( maxQuads, quadsThisFrame );
        if( f > 5 && clearsTotal == lastClears ) { skipped++; if( getenv("SKIPLOG") ) fprintf( stderr, "skip %d\n", f ); }
        lastClears = clearsTotal;
        if( f >= frameFrom && f <= frameTo && f % step == 0 )
        {
            char name[ 64 ]; snprintf( name, sizeof(name), "/f%04d.ppm", f );
            SavePPM( outdir + name );
        }
        if( console.IsCPUHalted() ) { fprintf( stderr, "CPU halted at frame %d\n", f ); break; }
    }
    if( audioFile ) fclose( audioFile );
    if( console.HasMemoryCard() && console.WasMemoryCardModified() ) console.SaveMemoryCard();
    printf( "frames=%d max_quads_per_frame=%d frames_without_new_clear=%d peak_audio=%ld\n", total, maxQuads, skipped, peakAudio );
    return 0;
}
