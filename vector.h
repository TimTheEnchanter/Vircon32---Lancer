// =============================================================================
//   vector.h - glowing vector graphics for Vircon32
// =============================================================================
//
//   Vircon32's GPU can only draw rectangular texture regions (optionally
//   scaled and rotated). This library draws lines by taking a 1-pixel-wide
//   strip of a texture, stretching it to the line's length and rotating it
//   to the line's angle. Each line is drawn twice: a soft, wide halo with
//   additive blending ("glow") and a thin, sharp line on top ("core").
//
//   REQUIREMENTS
//     - Put vector_lines.png (converted to .vtex) in your ROM's textures.
//     - Call vec_init( texture_id, first_region ) once at startup.
//       The library uses 3 regions: first_region .. first_region + 2.
//
//   FRAME USAGE
//     Line functions only queue lines. Call vec_flush() once per frame,
//     after queueing everything: it draws all glows first and then all
//     cores, so halos never wash over the sharp lines.
//     vec_fill_convex() draws immediately (call it before vec_flush()
//     to keep fills underneath your lines).
//
//   API SUMMARY
//     vec_init( texture, first_region )
//     vec_color( core_color, glow_color )      glow_color 0 = no glow
//     vec_thickness( pixels )                  core line width
//     vec_glow_size( pixels )                  halo width
//     vec_line( x1, y1, x2, y2 )
//     vec_line_polar( x, y, length, angle )    faster when you know these
//     vec_polyline( xs, ys, count, closed )
//     vec_shape( xs, ys, count, x, y, angle, scale )   transformed, closed
//     vec_shape_wrapped( ..., radius )         also draws screen-wrap copies
//     vec_arc( x, y, radius, start, end, segments )
//     vec_dashed_line( x1, y1, x2, y2, dash, gap )
//     vec_line_clipped( x1, y1, x2, y2 )       clipped to the screen first
//
//   3D (see the 3D section for the conventions)
//     vec_camera( center_x, center_y, focal, cam_x, cam_y, cam_z )
//     vec_project( x, y, z, &screen_x, &screen_y )   false if behind camera
//     vec_line_3d( x1, y1, z1, x2, y2, z2 )    near-plane and screen clipped
//     vec_depth_fade( near, far, min )         dim lines with distance
//     vec_circle( x, y, radius, segments )
//     vec_text( text, x, y, size )             built-in vector font
//     vec_number( value, x, y, size )
//     vec_text_width( text, size )
//     vec_text_centered( text, center_x, y, size )
//     vec_intensity( f )                       global fade, 0..1
//     vec_offset( x, y )                       camera offset / screen shake
//     vec_fill_convex( xs, ys, count, color )  software scanline fill
//     vec_fill_rect( x, y, w, h, color, blend ) solid / additive rectangle
//     vec_flush()
//     vec_queued_lines()                       lines queued this frame
//
//   Colors use the usual Vircon32 format, e.g. make_color_rgb( r, g, b ).
//   A good glow color is a darker version of the core color, since the
//   glow is added on top of whatever is behind it.
// =============================================================================

#ifndef VECTOR_H
#define VECTOR_H

#include "video.h"
#include "math.h"

// maximum number of lines that can be queued in one frame
#ifndef VEC_MAX_LINES
  #define VEC_MAX_LINES 700
#endif

// maximum points accepted by vec_shape / vec_circle
#define VEC_MAX_POINTS 64

// -----------------------------------------------------------------------------
//   INTERNAL STATE
// -----------------------------------------------------------------------------

int vec_texture_id = 0;
int vec_region_core = 0;
int vec_region_glow = 1;
int vec_region_pixel = 2;

int vec_current_core = 0xFFFFFFFF;
int vec_current_glow = 0;
int vec_base_core = 0xFFFFFFFF;
int vec_base_glow = 0;
float vec_current_intensity = 1.0;
float vec_offset_x = 0;
float vec_offset_y = 0;
float vec_current_thickness = 2.0;
float vec_current_glow_size = 14.0;

float[ VEC_MAX_LINES ] vec_q_x1;
float[ VEC_MAX_LINES ] vec_q_y1;
float[ VEC_MAX_LINES ] vec_q_length;
float[ VEC_MAX_LINES ] vec_q_angle;
float[ VEC_MAX_LINES ] vec_q_thickness;
float[ VEC_MAX_LINES ] vec_q_glow_size;
int[ VEC_MAX_LINES ] vec_q_core;
int[ VEC_MAX_LINES ] vec_q_glow;
int vec_q_count = 0;

float[ VEC_MAX_POINTS ] vec_tmp_x;
float[ VEC_MAX_POINTS ] vec_tmp_y;

// -----------------------------------------------------------------------------
//   SETUP
// -----------------------------------------------------------------------------

// texture layout expected (from vector_lines.png):
//   x = 0, y = 0..3   solid white strip  (line core)
//   x = 2, y = 0..31  gaussian strip     (line glow)
void vec_init_font();

void vec_init( int texture_id, int first_region )
{
    int previous_texture = get_selected_texture();

    vec_texture_id = texture_id;
    vec_region_core = first_region;
    vec_region_glow = first_region + 1;
    vec_region_pixel = first_region + 2;

    select_texture( texture_id );
    select_region( vec_region_core );
    define_region( 0, 0, 0, 3,  0, 2 );
    select_region( vec_region_glow );
    define_region( 2, 0, 2, 31,  2, 16 );
    select_region( vec_region_pixel );
    define_region( 0, 0, 0, 0,  0, 0 );

    select_texture( previous_texture );
    vec_q_count = 0;
    vec_init_font();
}

// multiplies a color's alpha (for line cores: fades them out)
// (both use bit operations directly: about 8x faster than going
// through the make_color / get_color functions)
int vec_scale_alpha( int color, float f )
{
    int a = ( ( color >> 24 ) & 255 ) * f;
    return ( color & 0x00FFFFFF ) | ( a << 24 );
}

// multiplies a color's RGB (for additive glows: dims them)
int vec_scale_rgb( int color, float f )
{
    int r = ( color & 255 ) * f;
    int g = ( ( color >> 8 ) & 255 ) * f;
    int b = ( ( color >> 16 ) & 255 ) * f;
    return ( color & 0xFF000000 ) | ( b << 16 ) | ( g << 8 ) | r;
}

void vec_update_colors()
{
    if( vec_current_intensity >= 1.0 )
    {
        vec_current_core = vec_base_core;
        vec_current_glow = vec_base_glow;
        return;
    }

    vec_current_core = vec_scale_alpha( vec_base_core, vec_current_intensity );

    if( vec_base_glow )
      vec_current_glow = vec_scale_rgb( vec_base_glow, vec_current_intensity );
    else
      vec_current_glow = 0;
}

// display filters: 0 as drawn, 1 amber, 2 green, 3 white, 4 a shifting spectrum.
// Each keeps a colour's brightest channel as its brightness
int vec_display = 0;
float vec_hue = 0;
int vec_fc_in = 1, vec_fc_out = 1, vec_fg_in = 1, vec_fg_out = 1;   // one-entry caches

int vec_filter_color( int c )
{
    int r, g, b, l;
    float p;
    if( vec_display == 0 || ( c & 0x00FFFFFF ) == 0 ) return c;
    r = c & 255;
    g = ( c >> 8 ) & 255;
    b = ( c >> 16 ) & 255;
    l = r;
    if( g > l ) l = g;
    if( b > l ) l = b;
    if( vec_display == 1 ) { r = l;  g = l * 0.68;  b = l * 0.18; }
    else if( vec_display == 2 ) { r = l * 0.3;  g = l;  b = l * 0.42; }
    else if( vec_display == 3 ) { r = l * 0.93;  g = l * 0.96;  b = l; }
    else
    {
        p = vec_hue + r * 0.011 + g * 0.017 + b * 0.023;
        r = l * ( 0.55 + 0.45 * sin( p ) );
        g = l * ( 0.55 + 0.45 * sin( p + 2.094 ) );
        b = l * ( 0.55 + 0.45 * sin( p + 4.189 ) );
    }
    return ( c & 0xFF000000 ) | ( b << 16 ) | ( g << 8 ) | r;
}

void vec_set_display( int display, float hue )
{
    vec_display = display;
    vec_hue = hue;
    vec_fc_in = 1;  vec_fc_out = vec_filter_color( 1 );
    vec_fg_in = 1;  vec_fg_out = vec_fc_out;
}

void vec_color( int core_color, int glow_color )
{
    if( vec_display != 0 )
    {
        if( core_color != vec_fc_in ) { vec_fc_in = core_color;  vec_fc_out = vec_filter_color( core_color ); }
        if( glow_color != vec_fg_in ) { vec_fg_in = glow_color;  vec_fg_out = vec_filter_color( glow_color ); }
        core_color = vec_fc_out;
        glow_color = vec_fg_out;
    }
    vec_base_core = core_color;
    vec_base_glow = glow_color;
    vec_update_colors();
}

// global brightness for everything queued from now on (0 = invisible,
// 1 = normal); useful for fades, blinking and title transitions
void vec_intensity( float f )
{
    if( f < 0 ) f = 0;
    if( f > 1 ) f = 1;
    vec_current_intensity = f;
    vec_update_colors();
}

// displacement added to everything queued from now on; set it
// to a small random amount for screen shake, back to 0 for HUDs
void vec_offset( float x, float y )
{
    vec_offset_x = x;
    vec_offset_y = y;
}

void vec_thickness( float pixels )
{
    vec_current_thickness = pixels;
}

void vec_glow_size( float pixels )
{
    vec_current_glow_size = pixels;
}

int vec_queued_lines()
{
    return vec_q_count;
}

// -----------------------------------------------------------------------------
//   LINES
// -----------------------------------------------------------------------------

void vec_line( float x1, float y1, float x2, float y2 )
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    int i;

    if( vec_q_count >= VEC_MAX_LINES )
      return;

    if( vec_current_intensity <= 0 )
      return;

    i = vec_q_count;
    vec_q_x1[ i ] = x1 + vec_offset_x;
    vec_q_y1[ i ] = y1 + vec_offset_y;
    vec_q_length[ i ] = sqrt( dx*dx + dy*dy ) + 1.0;
    // atan2( 0, 0 ) halts the Vircon32 CPU: a zero-length line (a 3D line
    // seen exactly end-on, say) is drawn as a dot instead
    if( dx == 0 && dy == 0 ) vec_q_angle[ i ] = 0;
    else vec_q_angle[ i ] = atan2( dy, dx );
    vec_q_thickness[ i ] = vec_current_thickness;
    vec_q_glow_size[ i ] = vec_current_glow_size;
    vec_q_core[ i ] = vec_current_core;
    vec_q_glow[ i ] = vec_current_glow;
    vec_q_count++;
}

// queues a line given by start point, length and angle (no sqrt/atan2)
void vec_line_polar( float x, float y, float length, float angle )
{
    int i;

    if( vec_q_count >= VEC_MAX_LINES || vec_current_intensity <= 0 )
      return;

    i = vec_q_count;
    vec_q_x1[ i ] = x + vec_offset_x;
    vec_q_y1[ i ] = y + vec_offset_y;
    vec_q_length[ i ] = length + 1.0;
    vec_q_angle[ i ] = angle;
    vec_q_thickness[ i ] = vec_current_thickness;
    vec_q_glow_size[ i ] = vec_current_glow_size;
    vec_q_core[ i ] = vec_current_core;
    vec_q_glow[ i ] = vec_current_glow;
    vec_q_count++;
}

void vec_polyline( float* xs, float* ys, int count, bool closed )
{
    int i;

    for( i = 0; i < count - 1; i++ )
      vec_line( xs[ i ], ys[ i ], xs[ i + 1 ], ys[ i + 1 ] );

    if( closed && count > 2 )
      vec_line( xs[ count - 1 ], ys[ count - 1 ], xs[ 0 ], ys[ 0 ] );
}

// draws a closed shape given in model coordinates, rotated by
// angle (radians, clockwise), scaled and placed at ( x, y )
void vec_shape( float* xs, float* ys, int count, float x, float y, float angle, float scale )
{
    float c = cos( angle ) * scale;
    float s = sin( angle ) * scale;
    float px, py, qx, qy, fx, fy, dx, dy;
    int i, q;

    if( count < 2 || vec_current_intensity <= 0 )
      return;

    // transforms points as it goes and writes the queue directly:
    // much cheaper than a vec_line call per edge
    fx = x + xs[ 0 ] * c - ys[ 0 ] * s;
    fy = y + xs[ 0 ] * s + ys[ 0 ] * c;
    px = fx;
    py = fy;

    for( i = 1; i <= count; i++ )
    {
        if( i < count )
        {
            qx = x + xs[ i ] * c - ys[ i ] * s;
            qy = y + xs[ i ] * s + ys[ i ] * c;
        }
        else
        {
            qx = fx;
            qy = fy;
        }

        q = vec_q_count;
        if( q >= VEC_MAX_LINES ) return;

        dx = qx - px;
        dy = qy - py;
        vec_q_x1[ q ] = px + vec_offset_x;
        vec_q_y1[ q ] = py + vec_offset_y;
        vec_q_length[ q ] = sqrt( dx*dx + dy*dy ) + 1.0;
        if( dx == 0 && dy == 0 ) vec_q_angle[ q ] = 0;      // see vec_line
        else vec_q_angle[ q ] = atan2( dy, dx );
        vec_q_thickness[ q ] = vec_current_thickness;
        vec_q_glow_size[ q ] = vec_current_glow_size;
        vec_q_core[ q ] = vec_current_core;
        vec_q_glow[ q ] = vec_current_glow;
        vec_q_count = q + 1;

        px = qx;
        py = qy;
    }
}

void vec_circle( float x, float y, float radius, int segments )
{
    float step;
    int i;

    if( segments > VEC_MAX_POINTS ) segments = VEC_MAX_POINTS;
    if( segments < 3 ) segments = 3;
    step = 6.2831853 / segments;

    for( i = 0; i < segments; i++ )
    {
        vec_tmp_x[ i ] = x + radius * cos( i * step );
        vec_tmp_y[ i ] = y + radius * sin( i * step );
    }

    vec_polyline( vec_tmp_x, vec_tmp_y, segments, true );
}

// same as vec_shape, but also draws the copies that peek in from the
// opposite edges when the shape overlaps a screen border (for objects
// that wrap around the screen); radius is the shape's bounding radius
void vec_shape_wrapped( float* xs, float* ys, int count, float x, float y,
                        float angle, float scale, float radius )
{
    float wx = 0, wy = 0;

    vec_shape( xs, ys, count, x, y, angle, scale );

    if( x < radius ) wx = screen_width;
    if( x > screen_width - radius ) wx = -screen_width;
    if( y < radius ) wy = screen_height;
    if( y > screen_height - radius ) wy = -screen_height;

    if( wx != 0 ) vec_shape( xs, ys, count, x + wx, y, angle, scale );
    if( wy != 0 ) vec_shape( xs, ys, count, x, y + wy, angle, scale );
    if( wx != 0 && wy != 0 ) vec_shape( xs, ys, count, x + wx, y + wy, angle, scale );
}

// arc from angle_start to angle_end (radians, clockwise)
void vec_arc( float x, float y, float radius, float angle_start, float angle_end, int segments )
{
    float step, a1, a2;
    int i;

    if( segments < 1 ) segments = 1;
    step = ( angle_end - angle_start ) / segments;

    for( i = 0; i < segments; i++ )
    {
        a1 = angle_start + i * step;
        a2 = a1 + step;
        vec_line( x + radius * cos( a1 ), y + radius * sin( a1 ),
                  x + radius * cos( a2 ), y + radius * sin( a2 ) );
    }
}

void vec_dashed_line( float x1, float y1, float x2, float y2, float dash, float gap )
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    float length = sqrt( dx*dx + dy*dy );
    float ux, uy, pos = 0, end;

    if( length < 0.5 ) return;
    ux = dx / length;
    uy = dy / length;

    while( pos < length )
    {
        end = fmin( pos + dash, length );
        vec_line( x1 + ux * pos, y1 + uy * pos, x1 + ux * end, y1 + uy * end );
        pos = end + gap;
    }
}

// -----------------------------------------------------------------------------
//   DRAWING THE QUEUE
// -----------------------------------------------------------------------------

void vec_flush()
{
    int i;
    int previous_texture = get_selected_texture();
    int last_color = 0;
    int color, px, py;
    float length, height, angle;

    select_texture( vec_texture_id );

    // the inner loops write GPU ports directly (the same ports video.h
    // uses), avoiding 4 function calls per line, and only send the
    // multiply color when it changes; this roughly halves the CPU time

    // pass 1: halos, added together
    set_blending_mode( blending_add );
    select_region( vec_region_glow );
    set_multiply_color( 0 );

    for( i = 0; i < vec_q_count; i++ )
    {
        color = vec_q_glow[ i ];
        if( color )
        {
            length = vec_q_length[ i ];
            height = vec_q_glow_size[ i ] / 32.0;
            angle = vec_q_angle[ i ];
            px = vec_q_x1[ i ];
            py = vec_q_y1[ i ];

            if( color != last_color )
            {
                asm
                {
                    "mov R0, {color}"
                    "out GPU_MultiplyColor, R0"
                }
                last_color = color;
            }

            asm
            {
                "mov R0, {length}"
                "out GPU_DrawingScaleX, R0"
                "mov R0, {height}"
                "out GPU_DrawingScaleY, R0"
                "mov R0, {angle}"
                "out GPU_DrawingAngle, R0"
                "mov R0, {px}"
                "out GPU_DrawingPointX, R0"
                "mov R0, {py}"
                "out GPU_DrawingPointY, R0"
                "out GPU_Command, GPUCommand_DrawRegionRotozoomed"
            }
        }
    }

    // pass 2: sharp lines on top
    set_blending_mode( blending_alpha );
    select_region( vec_region_core );
    set_multiply_color( 0 );
    last_color = 0;

    for( i = 0; i < vec_q_count; i++ )
    {
        color = vec_q_core[ i ];
        length = vec_q_length[ i ];
        height = vec_q_thickness[ i ] / 4.0;
        angle = vec_q_angle[ i ];
        px = vec_q_x1[ i ];
        py = vec_q_y1[ i ];

        if( color != last_color )
        {
            asm
            {
                "mov R0, {color}"
                "out GPU_MultiplyColor, R0"
            }
            last_color = color;
        }

        asm
        {
            "mov R0, {length}"
            "out GPU_DrawingScaleX, R0"
            "mov R0, {height}"
            "out GPU_DrawingScaleY, R0"
            "mov R0, {angle}"
            "out GPU_DrawingAngle, R0"
            "mov R0, {px}"
            "out GPU_DrawingPointX, R0"
            "mov R0, {py}"
            "out GPU_DrawingPointY, R0"
            "out GPU_Command, GPUCommand_DrawRegionRotozoomed"
        }
    }

    // leave GPU in a neutral state for other drawing code
    set_multiply_color( color_white );
    set_drawing_scale( 1, 1 );
    set_drawing_angle( 0 );
    select_texture( previous_texture );

    vec_q_count = 0;
}

// -----------------------------------------------------------------------------
//   FILLED CONVEX POLYGONS (drawn immediately, one span per scanline)
// -----------------------------------------------------------------------------

void vec_fill_convex( float* xs, float* ys, int count, int color )
{
    float min_y = 100000, max_y = -100000;
    float span_min, span_max, t, xi, ya, yb;
    int y, i, j, y_start, y_end;
    int previous_texture = get_selected_texture();

    for( i = 0; i < count; i++ )
    {
        min_y = fmin( min_y, ys[ i ] );
        max_y = fmax( max_y, ys[ i ] );
    }

    y_start = max( 0, ceil( min_y ) );
    y_end = min( screen_height - 1, floor( max_y ) );

    select_texture( vec_texture_id );
    select_region( vec_region_pixel );
    set_multiply_color( color );
    set_blending_mode( blending_alpha );
    set_drawing_angle( 0 );

    for( y = y_start; y <= y_end; y++ )
    {
        span_min = 100000;
        span_max = -100000;

        for( i = 0; i < count; i++ )
        {
            j = i + 1;
            if( j == count ) j = 0;
            ya = ys[ i ];
            yb = ys[ j ];

            if( ( y >= ya && y < yb ) || ( y >= yb && y < ya ) )
            {
                t = ( y - ya ) / ( yb - ya );
                xi = xs[ i ] + t * ( xs[ j ] - xs[ i ] );
                span_min = fmin( span_min, xi );
                span_max = fmax( span_max, xi );
            }
        }

        if( span_max > span_min )
        {
            set_drawing_scale( span_max - span_min + 1.0, 1.0 );
            draw_region_zoomed_at( span_min + vec_offset_x, y + vec_offset_y );
        }
    }

    set_multiply_color( color_white );
    set_drawing_scale( 1, 1 );
    select_texture( previous_texture );
}

// solid rectangle drawn immediately; blend_mode is blending_alpha
// (normal) or blending_add (e.g. a full-screen flash)
void vec_fill_rect( float x, float y, float w, float h, int color, int blend_mode )
{
    int previous_texture = get_selected_texture();

    select_texture( vec_texture_id );
    select_region( vec_region_pixel );
    set_multiply_color( color );
    set_blending_mode( blend_mode );
    set_drawing_angle( 0 );
    set_drawing_scale( w, h );
    draw_region_zoomed_at( x, y );

    set_blending_mode( blending_alpha );
    set_multiply_color( color_white );
    set_drawing_scale( 1, 1 );
    select_texture( previous_texture );
}

// -----------------------------------------------------------------------------
//   VECTOR FONT (16-segment style, plus 4 extra segments)
// -----------------------------------------------------------------------------
//
//   Character cell is 1 wide x 2 tall (times size). Segment endpoints are
//   given on a 3x3 grid of points:   0 1 2  (top)
//                                    3 4 5  (middle)
//                                    6 7 8  (bottom)

// segments 0-15 are the classic set; 16-19 (half top, half bottom and two
// diagonals into the middle right) give B and D their rounded shapes
int[ 20 ] vec_seg_a = { 0, 2, 5, 6, 3, 0, 3, 4, 0, 1, 2, 4, 4, 4, 3, 5, 0, 6, 1, 7 };
int[ 20 ] vec_seg_b = { 2, 5, 8, 8, 6, 3, 4, 5, 4, 4, 4, 6, 7, 8, 7, 7, 1, 7, 5, 5 };

// per-segment start point, length and angle for a size-1 cell,
// computed once by vec_init (text then needs no sqrt or atan2)
float[ 20 ] vec_seg_x;
float[ 20 ] vec_seg_y;
float[ 20 ] vec_seg_len;
float[ 20 ] vec_seg_ang;

void vec_init_font()
{
    int s;
    float ax, ay, bx, by;

    for( s = 0; s < 20; s++ )
    {
        ax = ( vec_seg_a[ s ] % 3 ) * 0.5;
        ay = ( vec_seg_a[ s ] / 3 );
        bx = ( vec_seg_b[ s ] % 3 ) * 0.5;
        by = ( vec_seg_b[ s ] / 3 );
        vec_seg_x[ s ] = ax;
        vec_seg_y[ s ] = ay;
        vec_seg_len[ s ] = sqrt( ( bx - ax ) * ( bx - ax ) + ( by - ay ) * ( by - ay ) );
        vec_seg_ang[ s ] = atan2( by - ay, bx - ax );
    }
}

// segment bitmasks for '0'-'9' then 'A'-'Z'
int[ 36 ] vec_font =
{
    0x0C3F, 0x1200, 0x00DB, 0x008F, 0x00E6, 0x00ED, 0x00FD, 0x0007, 0x00FF, 0x00EF,   // 0-9
    0x00F7, 0xA00F3, 0x0039, 0xF0030, 0x0079, 0x0071, 0x00BD, 0x00F6, 0x1209, 0x001E,   // A-J
    0x2470, 0x0038, 0x0536, 0x2136, 0x003F, 0x00F3, 0x203F, 0x20F3, 0x00ED, 0x1201,   // K-T
    0x003E, 0xC022, 0x2836, 0x2D00, 0x1500, 0x0C09                                    // U-Z
};

void vec_char( int c, float x, float y, float size )
{
    int mask = 0;
    int s;
    if( c >= 'a' && c <= 'z' ) c = c - 'a' + 'A';

    if( c >= '0' && c <= '9' )      mask = vec_font[ c - '0' ];
    else if( c >= 'A' && c <= 'Z' ) mask = vec_font[ c - 'A' + 10 ];
    else if( c == '-' )             mask = 0x00C0;
    else if( c == '+' )             mask = 0x12C0;
    else if( c == '/' )             mask = 0x0C00;
    else if( c == '?' )
    {
        // top, upper right and half the middle, then a short stem and a dot
        mask = 0x0083;
        vec_line( x + 0.5 * size, y + 1.0 * size, x + 0.5 * size, y + 1.45 * size );
        vec_line( x + 0.5 * size, y + 1.85 * size, x + 0.5 * size, y + 2.0 * size );
    }
    else if( c == '.' || c == ':' )
    {
        // small dots
        if( c == ':' ) vec_line( x + 0.5 * size, y + 0.5 * size, x + 0.5 * size, y + 0.6 * size );
        vec_line( x + 0.5 * size, y + 1.85 * size, x + 0.5 * size, y + 2.0 * size );
        return;
    }

    s = 0;
    while( mask )
    {
        if( mask & 1 )
          vec_line_polar( x + vec_seg_x[ s ] * size, y + vec_seg_y[ s ] * size,
                          vec_seg_len[ s ] * size, vec_seg_ang[ s ] );
        mask = mask >> 1;
        s++;
    }
}

// width in pixels of a text drawn with vec_text
float vec_text_width( int* text, float size )
{
    int n = 0;
    while( text[ n ] ) n++;
    if( n == 0 ) return 0;
    return ( n * 1.6 - 0.6 ) * size;
}

// draws text with its top-left corner at ( x, y );
// the cell is size wide and 2 * size tall
void vec_text( int* text, float x, float y, float size )
{
    while( *text )
    {
        vec_char( *text, x, y, size );
        x += 1.6 * size;
        text++;
    }
}

// text horizontally centered on center_x
void vec_text_centered( int* text, float center_x, float y, float size )
{
    vec_text( text, center_x - vec_text_width( text, size ) / 2, y, size );
}

void vec_number( int value, float x, float y, float size )
{
    int[ 12 ] digits;
    int n = 0, i;

    if( value < 0 )
    {
        vec_char( '-', x, y, size );
        x += 1.6 * size;
        value = -value;
    }

    do
    {
        digits[ n ] = '0' + value % 10;
        value /= 10;
        n++;
    }
    while( value > 0 );

    for( i = n - 1; i >= 0; i-- )
    {
        vec_char( digits[ i ], x, y, size );
        x += 1.6 * size;
    }
}

// -----------------------------------------------------------------------------
//   3D: A SIMPLE PERSPECTIVE CAMERA
// -----------------------------------------------------------------------------
//
//   World coordinates: x right, y down (like the screen), z away from the
//   viewer. The camera sits at ( cam_x, cam_y, cam_z ) looking along +z.
//   A point projects to
//       screen_x = center_x + focal * ( x - cam_x ) / ( z - cam_z )
//   Lines are clipped against a near plane in front of the camera and
//   then against the screen (with a margin), so that flying through
//   geometry never queues huge off-screen lines: the GPU charges every
//   draw by its stretched size, so those would eat the frame's budget.

float vec_cam_x = 0;
float vec_cam_y = 0;
float vec_cam_z = -1;
float vec_cam_focal = 160;
float vec_cam_cx = 320;
float vec_cam_cy = 180;
float vec_near = 0.05;

// depth fade: lines get dimmer with distance (off when far <= near)
float vec_fade_near = 0;
float vec_fade_far = 0;
float vec_fade_min = 1;

void vec_camera( float screen_center_x, float screen_center_y, float focal,
                 float cam_x, float cam_y, float cam_z )
{
    vec_cam_cx = screen_center_x;
    vec_cam_cy = screen_center_y;
    vec_cam_focal = focal;
    vec_cam_x = cam_x;
    vec_cam_y = cam_y;
    vec_cam_z = cam_z;
}

// distances are measured from the camera along z
void vec_depth_fade( float near_distance, float far_distance, float min_intensity )
{
    vec_fade_near = near_distance;
    vec_fade_far = far_distance;
    vec_fade_min = min_intensity;
}

// false when the point is behind the near plane
bool vec_project( float x, float y, float z, float* screen_x, float* screen_y )
{
    float d = z - vec_cam_z;
    if( d < vec_near ) return false;
    *screen_x = vec_cam_cx + vec_cam_focal * ( x - vec_cam_x ) / d;
    *screen_y = vec_cam_cy + vec_cam_focal * ( y - vec_cam_y ) / d;
    return true;
}

// clips a screen-space segment to the screen plus a margin (Liang-Barsky)
// and queues what is left
#define VEC_CLIP_MARGIN 32

void vec_line_clipped( float x1, float y1, float x2, float y2 )
{
    float dx = x2 - x1, dy = y2 - y1;
    float t0 = 0, t1 = 1, r;
    float[ 4 ] p;
    float[ 4 ] q;
    int i;

    p[ 0 ] = -dx;  q[ 0 ] = x1 + VEC_CLIP_MARGIN;
    p[ 1 ] = dx;   q[ 1 ] = screen_width + VEC_CLIP_MARGIN - x1;
    p[ 2 ] = -dy;  q[ 2 ] = y1 + VEC_CLIP_MARGIN;
    p[ 3 ] = dy;   q[ 3 ] = screen_height + VEC_CLIP_MARGIN - y1;

    for( i = 0; i < 4; i++ )
    {
        if( p[ i ] == 0 )
        {
            if( q[ i ] < 0 ) return;          // parallel and outside
        }
        else
        {
            r = q[ i ] / p[ i ];
            if( p[ i ] < 0 )
            {
                if( r > t1 ) return;
                if( r > t0 ) t0 = r;
            }
            else
            {
                if( r < t0 ) return;
                if( r < t1 ) t1 = r;
            }
        }
    }

    vec_line( x1 + dx * t0, y1 + dy * t0, x1 + dx * t1, y1 + dy * t1 );
}

void vec_line_3d( float x1, float y1, float z1, float x2, float y2, float z2 )
{
    float d1 = z1 - vec_cam_z;
    float d2 = z2 - vec_cam_z;
    float t, f, sx1, sy1, sx2, sy2;
    int saved_core, saved_glow;

    if( d1 < vec_near && d2 < vec_near ) return;

    // cut away the part behind the near plane
    if( d1 < vec_near )
    {
        t = ( vec_near - d1 ) / ( d2 - d1 );
        x1 += ( x2 - x1 ) * t;
        y1 += ( y2 - y1 ) * t;
        d1 = vec_near;
    }
    else if( d2 < vec_near )
    {
        t = ( vec_near - d2 ) / ( d1 - d2 );
        x2 += ( x1 - x2 ) * t;
        y2 += ( y1 - y2 ) * t;
        d2 = vec_near;
    }

    sx1 = vec_cam_cx + vec_cam_focal * ( x1 - vec_cam_x ) / d1;
    sy1 = vec_cam_cy + vec_cam_focal * ( y1 - vec_cam_y ) / d1;
    sx2 = vec_cam_cx + vec_cam_focal * ( x2 - vec_cam_x ) / d2;
    sy2 = vec_cam_cy + vec_cam_focal * ( y2 - vec_cam_y ) / d2;

    if( vec_fade_far > vec_fade_near )
    {
        f = 1.0 - ( ( d1 + d2 ) * 0.5 - vec_fade_near ) / ( vec_fade_far - vec_fade_near );
        if( f < vec_fade_min ) f = vec_fade_min;

        if( f < 1.0 )
        {
            saved_core = vec_current_core;
            saved_glow = vec_current_glow;
            vec_current_core = vec_scale_alpha( saved_core, f );
            if( saved_glow ) vec_current_glow = vec_scale_rgb( saved_glow, f );
            vec_line_clipped( sx1, sy1, sx2, sy2 );
            vec_current_core = saved_core;
            vec_current_glow = saved_glow;
            return;
        }
    }

    vec_line_clipped( sx1, sy1, sx2, sy2 );
}

#endif
