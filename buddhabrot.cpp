/*  Nebulabrot / Buddhabrot generator.
    http://en.wikipedia.org/wiki/User_talk:Michael.Pohoreski/Buddhabrot.cpp

    Optimized and cleaned up version by Michael Pohoreski
    Based on the original version by Evercat

        g++ -Wall -O2 buddhabrot.cpp -o buddhabrot
        ./bin/buddhabrot 3000 4000 20000

   Released under the GNU Free Documentation License
   or the GNU Public License, whichever you prefer.
*/

// Includes
    #include <stdio.h>
    #include <stdlib.h>
    #include <math.h>
    #include <stdint.h> // uint16_t uint32_t
    #include <string.h> // memset()

// Macros
    #define VERBOSE if(gbVerbose)

    #define reduce(x) (x)       // Macro to reduce greyscale, can use sqrt(n), log(n), etc, or just (n)

// Globals

    // Input parameters
    double    gnWorldMinX        = -2.102613; // WorldW = MaxX-MinX = 3.303226
    double    gnWorldMaxX        =  1.200613;
    double    gnWorldMinY        = -1.237710; // WorldH = MaxY-MinY = 2.47742 
    double    gnWorldMaxY        =  1.239710;

    int       gnMaxDepth         = 1000; // max number of iterations == # of pixels to plot per complex number
    int       gnWidth            = 1024; // image width
    int       gnHeight           =  768; // image height
    int       gnScale            =   10;

    bool      gbAutoBrightness   = false;
    // Default MaxDepth = 1000 @ 1042x768 has a maximum greyscale intensity = 5010 -> 230/5010 = filter out bottom 4.590808% of image as black
    int       gnGreyscaleBias    = -230; // color pixel = (greyscale pixel + bias) * scale = 5010 - 230 = 4780

    float     gnScaleR           = 0.09; // Default: (5010 - 230) * 0.09 = 430.2
    float     gnScaleG           = 0.11; // Default: (5010 - 230) * 0.11 = 525.8
    float     gnScaleB           = 0.18; // Default: (5010 - 230) * 0.18 = 860.4

    bool      gbVerbose          = false;
    bool      gbSaveRawGreyscale = true ;

    // Calculated/Cached
    uint32_t  gnImageArea        =    0; // image width * image height

    // Output
    uint16_t *gpGreyscaleTexels  = NULL; // [ height ][ width ] 16-bit greyscale
    uint8_t  *gpChromaticTexels  = NULL; // [ height ][ width ] 24-bit RGB


// Timer___________________________________________________________________________ 

#ifdef _WIN32 // MSC_VER
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <Windows.h> // Windows.h -> WinDef.h defines min() max()

    /*
        typedef uint16_t WORD ;
        typedef uint32_t DWORD;

        typedef struct _FILETIME {
            DWORD dwLowDateTime;
            DWORD dwHighDateTime;
        } FILETIME;

        typedef struct _SYSTEMTIME {
              WORD wYear;
              WORD wMonth;
              WORD wDayOfWeek;
              WORD wDay;
              WORD wHour;
              WORD wMinute;
              WORD wSecond;
              WORD wMilliseconds;
        } SYSTEMTIME, *PSYSTEMTIME;
    */

    // *sigh* Microsoft has this in winsock2.h because they are too lazy to put it in the standard location ... !?!?
    typedef struct timeval {
        long tv_sec;
        long tv_usec;
    } timeval;

    // *sigh* no gettimeofday on Win32/Win64
    int gettimeofday(struct timeval * tp, struct timezone * tzp)
    {
        // FILETIME Jan 1 1970 00:00:00
        // Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
        static const uint64_t EPOCH = ((uint64_t) 116444736000000000ULL); 

        SYSTEMTIME  nSystemTime;
        FILETIME    nFileTime;
        uint64_t    nTime;

        GetSystemTime( &nSystemTime );
        SystemTimeToFileTime( &nSystemTime, &nFileTime );
        nTime =  ((uint64_t)nFileTime.dwLowDateTime )      ;
        nTime += ((uint64_t)nFileTime.dwHighDateTime) << 32;

        tp->tv_sec  = (long) ((nTime - EPOCH) / 10000000L);
        tp->tv_usec = (long) (nSystemTime.wMilliseconds * 1000);
        return 0;
    }
#else
    #include <sys/time.h>
#endif // _WIN32

    struct DataRate
    {
        char     prefix ;
        uint64_t samples;
        uint64_t per_sec;
    };

    class Timer
    {
        timeval start, end; // Windows: winsock2.h  Unix: sys/time.h 
    public:
        double   elapsed; // total seconds
        uint32_t mins;
        uint32_t secs;
        DataRate throughput;

        void Start()
        {
            gettimeofday( &start, NULL );
        }

        void Stop()
        {
            gettimeofday( &end, NULL );
            elapsed = (end.tv_sec - start.tv_sec);

            mins = (uint32_t)elapsed / 60;
            secs = (uint32_t)elapsed - (mins*60);
        }

        // size is number of bytes in a file, or number of iterations that you want to benchmark
        void Throughput( uint64_t size )
        {
            const int MAX_PREFIX = 4;
            DataRate datarate[ MAX_PREFIX ] = {
                {' ',0,0}, {'K',0,0}, {'M',0,0}, {'G',0,0} // 1; 1,000; 1,000,000; 1,000,000,000
            };

            if( !elapsed )
                return;

            int best = 0;
            for( int units = 0; units < MAX_PREFIX; units++ )
            {
                    datarate[ units ].samples = size >> (10*units);
                    datarate[ units ].per_sec = (uint64_t) (datarate[units].samples / elapsed);
                if (datarate[ units ].per_sec > 0)
                    best = units;
            }
            throughput = datarate[ best ];
        }
    };


// Implementation _________________________________________________________________ 

// ========================================================================
void AllocImageMemory( const int width, const int height )
{
    const size_t nArea           = width * height;

    const size_t nGreyscaleBytes = nArea  * sizeof( uint16_t );
    gpGreyscaleTexels = (uint16_t*) malloc( nGreyscaleBytes );   // 1x 16-bit channel: K
    memset( gpGreyscaleTexels, 0, nGreyscaleBytes );

    const size_t chromaticBytes  = nArea * 3 * sizeof( uint8_t ); // 3x 8-bit channels: R,G,B
    gpChromaticTexels = (uint8_t*) malloc( chromaticBytes );
    memset( gpChromaticTexels, 0, chromaticBytes );
}


// ========================================================================
void BMP_WriteColor24bit( const char * filename, const uint8_t *texelsRGB, const int width, const int height )
{
    uint32_t headers[13]; // 54 bytes == 13 x int32
    FILE   * pFileSave;
    int x, y, i;

    // Stupid Windows BMP must have each scanline width padded to 4 bytes
    int      nExtraBytes = (width * 3) % 4;
    int      nPaddedSize = (width * 3 + nExtraBytes) * height;
    uint32_t nPlanes     =  1      ; // 1 plane
    uint32_t nBitcount   = 24 << 16; // 24-bit RGB; 32-bit packed for writing

    // Header: Note that the "BM" identifier in bytes 0 and 1 is NOT included in these "headers".
    headers[ 0] = nPaddedSize + 54;    // bfSize (total file size)
    headers[ 1] = 0;                   // bfReserved1 bfReserved2
    headers[ 2] = 54;                  // bfOffbits
    headers[ 3] = 40;                  // biSize BITMAPHEADER
    headers[ 4] = width;               // biWidth
    headers[ 5] = height;              // biHeight
    headers[ 6] = nBitcount | nPlanes; // biPlanes, biBitcount
    headers[ 7] = 0;                   // biCompression
    headers[ 8] = nPaddedSize;         // biSizeImage
    headers[ 9] = 0;                   // biXPelsPerMeter
    headers[10] = 0;                   // biYPelsPerMeter
    headers[11] = 0;                   // biClrUsed
    headers[12] = 0;                   // biClrImportant

    pFileSave = fopen(filename, "wb");
    if( pFileSave )
    {
        // Output Headers
        fprintf(pFileSave, "BM");
        for( i = 0; i < 13; i++ )
        {
           fprintf( pFileSave, "%c", ((headers[i]) >>  0) & 0xFF );
           fprintf( pFileSave, "%c", ((headers[i]) >>  8) & 0xFF );
           fprintf( pFileSave, "%c", ((headers[i]) >> 16) & 0xFF );
           fprintf( pFileSave, "%c", ((headers[i]) >> 24) & 0xFF );
        }

        // Stupid Windows BMP are written upside down
        for( y = height - 1; y >= 0; y-- )
        {
            const uint8_t* scanline = &texelsRGB[ y*width*3 ];
            for( x = 0; x < width; x++ )
            {
                // swizzle rgb -> brg
                uint8_t r = *scanline++;
                uint8_t g = *scanline++;
                uint8_t b = *scanline++;

                // Stupid Windows BMP are written BGR
                fprintf( pFileSave, "%c", b );
                fprintf( pFileSave, "%c", g );
                fprintf( pFileSave, "%c", r );
           }

           if( nExtraBytes ) // See above - BMP lines must be of lengths divisible by 4 bytes.
              for( i = 0; i < nExtraBytes; i++ )
                 fprintf( pFileSave, "%c", 0 );
        }

        fclose( pFileSave );
    }
}


// Scan all pixels and return the maximum brightness
// ========================================================================
uint16_t
Image_Greyscale16bitMaxValue( const uint16_t *texels, const int width, const int height )
{
    const uint16_t *pSrc = texels;
    const int       nLen = width * height;
    /* */ int       nMax = *pSrc;

    for( int iPix = 0; iPix < nLen; iPix++ )
    {
        if( nMax < *pSrc )
            nMax = *pSrc;
        pSrc++;
    }

    return nMax;
}


// ========================================================================
uint16_t
Image_Greyscale16bitToBrightnessBias( int* bias_, float* scaleR_, float* scaleG_, float* scaleB_ )
{
    uint16_t nMaxBrightness = Image_Greyscale16bitMaxValue( gpGreyscaleTexels, gnWidth, gnHeight );
    VERBOSE printf( "Max brightness: %d\n", nMaxBrightness );

    if( gbAutoBrightness )
    {
        if( nMaxBrightness < 256)
            *bias_ = 0;

        // TODO: if bright < 256 should this be adjusted?
        *bias_ = (int)(-0.045 * nMaxBrightness); // low-pass noise filter; if greyscale pixel < bias then greyscale pixel = 0

        *scaleR_ = 430. / (float)nMaxBrightness;
        *scaleG_ = 525. / (float)nMaxBrightness;
        *scaleB_ = 860. / (float)nMaxBrightness;
    }

    return nMaxBrightness;
}


// @param greyscale  Source greyscale texels to read
// @param chromatic_ Destination chromatic texels to write
// ========================================================================
void
Image_Greyscale16bitToColor24bit(
    const uint16_t* greyscale, const int width, const int height,
    /* */ uint8_t * chromatic_,
    const int bias, const double scaleR, const double scaleG, const double scaleB )
{
    const int       nLen = width * height;
    const uint16_t *pSrc = greyscale;
    /* */ uint8_t  *pDst = chromatic_;

    for( int iPix = 0; iPix < nLen; iPix++ )
    {
        int i = *pSrc++ + bias  ; // low pass noise filter
        int r = (int)(i * scaleR);
        int g = (int)(i * scaleG);
        int b = (int)(i * scaleB);

        if (r > 255) r = 255; if (r < 0) r = 0;
        if (g > 255) g = 255; if (g < 0) g = 0;
        if (b > 255) b = 255; if (b < 0) b = 0;

        *pDst++ = r;
        *pDst++ = g;
        *pDst++ = b;
    }
}


// ========================================================================
void
RAW_WriteGreyscale16bit( const char *filename, const uint16_t *texels, const int width, const int height )
{
    FILE *file = fopen( filename, "wb" );
    if( file )
    {
        const size_t area = width * height;
        fwrite( texels, sizeof( uint16_t ), area, file );
        fclose( file );
    }
}


// @param wx World X start location
// @param wy World Y start location
// @param sx World to Image scale X
// @param sy World to Image scale Y
// ========================================================================
inline
void plot( double wx, double wy, double sx, double sy, uint16_t *texels, const int width, const int height, const int maxdepth )
{
    double  r = 0., i = 0.; // Zn   current Complex< real, imaginary >
    double  s    , j      ; // Zn+1 next    Complex< real, imaginary >
    int     u     , v     ; // texel coords

    for( int depth = 0; depth < maxdepth; depth++ )
    {
        s = (r*r - i*i) + wx;
        j = (2*r*i)     + wy;

        r = s;
        i = j;

        if ((r*r + i*i) > 4.0 ) // escapes to infinity, don't render
            return;

        u = (int) ((r - gnWorldMinX) * sx); // texel x
        v = (int) ((i - gnWorldMinY) * sy); // texel y

        if( u < width && v < height && u >= 0 && v >= 0 )
            texels[ (v * width) + u ]++;
    }
}


// @return Number of input scaled pixels (Not uber total of all pixels processed)
// ========================================================================
int Buddhabrot()
{
    if( gnScale < 0)
        gnScale = 1;

    const int nCol = gnWidth  * gnScale ; // scaled width
    const int nRow = gnHeight * gnScale ; // scaled height

    /* */ int iCel = 0                  ; // Progress status for percent compelete
    const int nCel = nCol     * nRow    ; // scaled width  * scaled height;

    const double nWorldW = gnWorldMaxX - gnWorldMinX;
    const double nWorldH = gnWorldMaxY - gnWorldMinY;

    // Map Source (world space) to Pixels (image space)
    const double nWorld2ImageX = (double)(gnWidth  - 1.) / nWorldW;
    const double nWorld2ImageY = (double)(gnHeight - 1.) / nWorldH;

    const double dx = nWorldW / (nCol - 1.0);
    const double dy = nWorldH / (nRow - 1.0);

    for( int iCol = 0; iCol < nCol; iCol++ )
    {
        const double x = gnWorldMinX + (dx*iCol);

        for( int iRow = 0; iRow < nRow; iRow++ )
        {
            const double y = gnWorldMinY + (dy*iRow);

            iCel++;

            double r = 0., i = 0., s, j;
            for (int depth = 0; depth < gnMaxDepth; depth++)
            {
                s = ((r*r) - (i*i)) + x; // Zn+1 = Zn^2 + C<x,y>
                j = (2*r*i)         + y;

                r = s;
                i = j;

                if ((r*r + i*i) > 4.0) // escapes to infinity so trace path
                {
                    plot( x, y, nWorld2ImageX, nWorld2ImageY, gpGreyscaleTexels, gnWidth, gnHeight, gnMaxDepth );
                    break;
                }
            }
        }

        VERBOSE
        {
                const double percent = (100.0  * iCel) / nCel;

                for( int i = 0; i < 40; i++ )
                    printf( "%c", 8 ); // ASCII backspace

                printf( "%6.2f%% = %d / %d", percent, iCel, nCel );
                fflush( stdout );
        }
    }

    return nCel;
}


// ========================================================================
int Usage()
{
    printf(
"Buddhabrot v1.8 by Michael Pohoreski\n"
"Usage: [width height depth]\n"
"\n"
"-?   Dipslay usage help\n"
"-b   Use auto brightness\n"
"-r   Save raw image\n"
"-v   Verbose.  Display %% complete\n"
    );
    return 0;
}


// ========================================================================
int main( int nArg, char * aArg[] )
{
    int   iArg = 0;
    char *pArg;

    if( nArg > 1 )
    {
        while( iArg < nArg )
        {
            if( aArg[iArg+1] && aArg[iArg+1][0] == '-' )
            {
                iArg++;
                pArg = &aArg[ iArg ][1]; // point to 1st char in option
                if( *pArg == '?' )
                    return Usage();
                else
                if( *pArg == 'b' )
                    gbAutoBrightness = true;
                else
                if( *pArg == 'r' )
                    gbSaveRawGreyscale = true;
                else
                if( *pArg == 'v' )
                    gbVerbose = true;
                else
                    printf( "Unrecognized option: %c\n", *pArg ); 
            }
            else
                break;
        }
    }
    // iArg is index to first non-flag

    if ((iArg+1) < nArg) gnWidth    = atoi( aArg[iArg+1] );
    if ((iArg+2) < nArg) gnHeight   = atoi( aArg[iArg+2] );
    if ((iArg+3) < nArg) gnMaxDepth = atoi( aArg[iArg+3] );
    if ((iArg+4) < nArg) gnScale    = atoi( aArg[iArg+4] );

    if( !iArg )
        printf( "Width: %d  Height: %d  Depth: %d  Scale: %d\n", gnWidth, gnHeight, gnMaxDepth, gnScale );

    AllocImageMemory( gnWidth, gnHeight );

    Timer stopwatch;
    stopwatch.Start();
        int nCells = Buddhabrot();
    stopwatch.Stop();
    stopwatch.Throughput( nCells ); // Calculate throughput in pixels/s
    printf( "%d %cpix/s (%d pixels, %.f seconds = %d:%d)\n"
        , (int)stopwatch.throughput.per_sec, stopwatch.throughput.prefix
        , nCells
        , stopwatch.elapsed
        , stopwatch.mins, stopwatch.secs
    );

    VERBOSE printf( "\n" );

    if( gbSaveRawGreyscale )
    {
        char     filenameRAW[ 256 ];
        sprintf( filenameRAW, "cpu1_buddhabrot_%dx%d_%d_%dx.u16.data", gnWidth, gnHeight, gnMaxDepth, gnScale );

        RAW_WriteGreyscale16bit( filenameRAW, gpGreyscaleTexels, gnWidth, gnHeight );
        printf( "Saved: %s\n", filenameRAW );
    }

    char     filenameBMP[256];
#if DEBUG
    sprintf( filenameBMP, "cpu1_buddhabrot_%dx%d_depth_%d_colorscaling_%d_scale_%dx.bmp", gnWidth, gnHeight, gnMaxDepth, (int)gbAutoBrightness, gnScale );
#else
    sprintf( filenameBMP, "cpu1_buddhabrot_%dx%d@%d.bmp", gnWidth, gnHeight, gnMaxDepth );
#endif

    Image_Greyscale16bitToBrightnessBias( &gnGreyscaleBias, &gnScaleR, &gnScaleG, &gnScaleB ); // don't need max brightness
    Image_Greyscale16bitToColor24bit( gpGreyscaleTexels, gnWidth, gnHeight, gpChromaticTexels, gnGreyscaleBias, gnScaleR, gnScaleG, gnScaleB );
    BMP_WriteColor24bit( filenameBMP, gpChromaticTexels, gnWidth, gnHeight );
    printf( "Saved: %s\n", filenameBMP );

    return 0;
}