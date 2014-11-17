//
// begin license header
//
// This file is part of Pixy CMUcam5 or "Pixy" for short
//
// All Pixy source code is provided under the terms of the
// GNU General Public License v2 (http://www.gnu.org/licenses/gpl-2.0.html).
// Those wishing to use Pixy source code, software and/or
// technologies under different licensing terms should contact us at
// cmucam@cs.cmu.edu. Such licensing terms are available for
// all portions of the Pixy codebase presented here.
//
// end license header
//

#include <stdio.h>
#include <debug.h>
#include <string.h>
#include <math.h>
#include "pixy_init.h"
#include "camera.h"
#include "cameravals.h"
#include "param.h"
#include "conncomp.h"
#include "blobs.h"
#include "led.h"
#include "qqueue.h"


Qqueue *g_qqueue;
Blobs *g_blobs;

int g_loop = 0;

int32_t cc_servo(const uint32_t &start)
{
	g_loop = start;
	return 0;
}

static const ProcModule g_module[] =
{
	{
	"cc_getRLSFrame",
	(ProcPtr)cc_getRLSFrameChirp, 
	{END}, 
	"Get a frame of color run-length segments (RLS)"
	"@r 0 if success, negative if error"
	"@r CCQ1 formated data, including 8-palette"
	},
	{
	"cc_setSigRegion",
	(ProcPtr)cc_setSigRegion, 
	{CRP_UINT32, CRP_UINT8, CRP_HTYPE(FOURCC('R','E','G','1')), END}, 
	"Set signature by selecting region in image"
	"@p type 0=normal signature, 1=color code signature"
	"@p signature numerical index of signature, can be 1-7"
	"@p region user-selected region"
	"@r 0 to 100 if success where 100=good, 0=poor, negative if error"
	},
	{
	"cc_setSigPoint",
	(ProcPtr)cc_setSigPoint, 
	{CRP_UINT32, CRP_UINT8, CRP_HTYPE(FOURCC('P','N','T','1')), END}, 
	"Set signature by selecting point in image"
	"@p type 0=normal signature, 1=color code signature"
	"@p signature numerical index of signature, can be 1-7"
	"@p point user-selected point"
	"@r 0 to 100 if success where 100=good, 0=poor, negative if error"
	},
	{
	"cc_clearSig",
	(ProcPtr)cc_clearSig, 
	{CRP_UINT8, END}, 
	"Clear signature"
	"@p signature numerical index of signature, can be 1-7"
	"@r 0 if success, negative if error"
	},
	{
	"cc_clearAllSig",
	(ProcPtr)cc_clearAllSig, 
	{END}, 
	"Clear signature"
	"@r 0 if success, negative if error"
	},
	{
	"cc_setMemory",
	(ProcPtr)cc_setMemory,
	{CRP_UINT32, CRP_UINTS8, END},
	"" 
	},
	END
};

const uint32_t g_colors[] = 
{
	0xffffff, // 0 white
	0xff0000, // 1 red
	0xff8000, // 2 orange
	0xffff00, // 3 yellow
	0x00ff00, // 4 green
	0x00ffff, // 5 cyan
	0x0000ff, // 6 blue
	0xff00ff  // 7 violet
};

static ChirpProc g_getRLSFrameM0 = -1;

int cc_loadLut(void)
{
	int i, res;
	uint32_t len;
	char id[32];
	ColorSignature *psig;

	for (i=1; i<=CL_NUM_SIGNATURES; i++)
	{
		sprintf(id, "signature%d", i);
		// get signature and add to color lut
		res = prm_get(id, &len, &psig, END);
		if (res<0)
			return res;
		g_blobs->m_clut.setSignature(i, *psig);
	}

	g_blobs->m_clut.generateLUT();
	// go ahead and flush since we've changed things
	g_qqueue->flush();

	return 0;
}

void cc_loadParams(void)
{
	int i;
	ColorSignature signature;
	char id[32], desc[32];

	// set up signatures, load later
	for (i=1; i<=CL_NUM_SIGNATURES; i++)
	{
		sprintf(id, "signature%d", i);
		sprintf(desc, "Color signature %d", i);
		// add if it doesn't exist yet
		prm_add(id, PRM_FLAG_INTERNAL, desc, INTS8(sizeof(ColorSignature), &signature), END);
	}

	// others -----

	// setup
	prm_add("Max blocks", 0, 
		"Sets the maximum total blocks sent per frame. (default 1000)", UINT16(1000), END);
	prm_add("Max blocks per signature", 0, 
		"Sets the maximum blocks for each color signature sent for each frame. (default 1000)", UINT16(1000), END);
	prm_add("Min block area", 0, 
		"Sets the minimum required area in pixels for a block.  Blocks with less area won't be sent. (default 20)", UINT32(20), END);
	prm_add("Color code mode", 0,
		"Sets the color code mode, 0=disabled, 1=enabled, 2=color codes only, 3=mixed (default 1)", INT8(1), END);

	// load
	uint8_t ccMode;
	uint16_t maxBlobs, maxBlobsPerModel;
	uint32_t minArea;

	prm_get("Max blocks", &maxBlobs, END);
	prm_get("Max blocks per signature", &maxBlobsPerModel, END);
	prm_get("Min block area", &minArea, END);
	prm_get("Color code mode", &ccMode, END);
	g_blobs->setParams(maxBlobs, maxBlobsPerModel, minArea, (ColorCodeMode)ccMode);

	cc_loadLut();
}

int cc_init(Chirp *chirp)
{
	g_qqueue = new Qqueue;
	g_blobs = new Blobs(g_qqueue, LUT_MEMORY);

	chirp->registerModule(g_module);	

	g_getRLSFrameM0 = g_chirpM0->getProc("getRLSFrame", NULL);

	if (g_getRLSFrameM0<0)
		return -1;

	cc_loadParams(); // setup default vals and load parameters

	return 0;
}

void cc_setBounds(const uint8_t mode)
{
#ifdef DEFER
	float minSat, hueTol, satTol;

	if (mode==1)
	{
		prm_get("CC min saturation", &minSat, END);
		prm_get("CC hue spread", &hueTol, END);
		prm_get("CC saturation spread", &satTol, END);
	}
	else
	{
		prm_get("Min saturation", &minSat, END);
		prm_get("Hue spread", &hueTol, END);
		prm_get("Saturation spread", &satTol, END);
	}

   	g_blobs->m_clut->setBounds(minSat, hueTol, satTol); 
#endif
}

// this routine assumes it can grab valid pixels in video memory described by the box
int32_t cc_setSigRegion(const uint32_t &type, const uint8_t &signum, const uint16_t &xoffset, const uint16_t &yoffset, const uint16_t &width, const uint16_t &height)
{
	char id[32];
	ColorSignature *sig;

	if (signum<1 || signum>CL_NUM_SIGNATURES)
		return -1;

	if (g_rawFrame.m_pixels==NULL)
	{
		cprintf("No raw frame in memory!\n");
		return -2;
	}

	// create lut
	g_blobs->m_clut.generateSignature(g_rawFrame, RectA(xoffset, yoffset, width, height), signum);
	sig = g_blobs->m_clut.getSignature(signum);
	g_blobs->m_clut.generateLUT();
	sig->m_type = type;

	// save to flash
	sprintf(id, "signature%d", signum);
	prm_set(id, INTS8(sizeof(ColorSignature), sig), END);

	cprintf("Success!\n");

	return 0;
}

int32_t cc_setSigPoint(const uint32_t &type, const uint8_t &signum, const uint16_t &x, const uint16_t &y, Chirp *chirp)
{
	char id[32];
	ColorSignature *sig;
	Points points;

	if (signum<1 || signum>CL_NUM_SIGNATURES)
		return -1;

	if (g_rawFrame.m_pixels==NULL)
	{
		cprintf("No raw frame in memory!\n");
		return -2;
	}

	// create lut
	g_blobs->m_clut.generateSignature(g_rawFrame, Point16(x, y), &points, signum);
	sig = g_blobs->m_clut.getSignature(signum);
	g_blobs->m_clut.generateLUT();
	sig->m_type = type;

	cc_sendPoints(points, CL_GROW_INC, CL_GROW_INC, chirp);

	// save to flash
	sprintf(id, "signature%d", signum);
	prm_set(id, INTS8(sizeof(ColorSignature), sig), END);

	//cprintf("Success!\n");

	return 0;
}

int32_t cc_clearSig(const uint8_t &signum)
{
	char id[32];
	ColorSignature sig;
	int res;

 	if (signum<1 || signum>CL_NUM_SIGNATURES)
		return -1;

	memset(&sig, 0, sizeof(ColorSignature));

	sprintf(id, "signature%d", signum);
	res = prm_set(id, INTS8(sizeof(ColorSignature), &sig), END);

	// update lut
 	cc_loadLut();

	return res;
}

int32_t cc_clearAllSig()
{
	char id[32];
	uint8_t signum;
	ColorSignature sig;
	int res; 

	memset(&sig, 0, sizeof(ColorSignature));

   	for (signum=1; signum<=CL_NUM_SIGNATURES; signum++)
	{
		sprintf(id, "signature%d", signum);
		res = prm_set(id, INTS8(sizeof(ColorSignature), &sig), END);
		if (res<0)
			return res;			
	}

	// update lut
 	cc_loadLut();
	return 0;
}


int32_t cc_getRLSFrameChirp(Chirp *chirp)
{
	return cc_getRLSFrameChirpFlags(chirp);
}

#define LUT_SIZE  0x1000

int32_t cc_getRLSFrameChirpFlags(Chirp *chirp, uint8_t renderFlags)
{

#if 0
	int32_t result;
	uint32_t len, numRls;

	if (g_rawFrame.m_pixels)
		cc_loadLut();

	g_qqueue->flush();

	// figure out prebuf length (we need the prebuf length and the number of runlength segments, but there's a chicken and egg problem...)
	len = Chirp::serialize(chirp, RLS_MEMORY, RLS_MEMORY_SIZE,  HTYPE(0), UINT16(0), UINT16(0), UINTS32_NO_COPY(0), END);

	result = cc_getRLSFrame((uint32_t *)(RLS_MEMORY+len), LUT_MEMORY);
	// copy from IPC memory to RLS_MEMORY
	numRls = g_qqueue->readAll((Qval *)(RLS_MEMORY+len), (RLS_MEMORY_SIZE-len)/sizeof(Qval));
	Chirp::serialize(chirp, RLS_MEMORY, RLS_MEMORY_SIZE,  HTYPE(FOURCC('C','C','Q','1')), HINT8(renderFlags), UINT16(CAM_RES2_WIDTH), UINT16(CAM_RES2_HEIGHT), UINTS32_NO_COPY(numRls), END);
	// send frame, use in-place buffer
	chirp->useBuffer(RLS_MEMORY, len+numRls*4);
#else
#define MAX_NEW_QVALS_PER_LINE   ((CAM_RES2_WIDTH/3)+2)

	int32_t result;
	uint8_t *scratchMem, *lut, *mem;
	uint32_t len, memSize, numq;


	lut = (uint8_t *)SRAM1_LOC + SRAM1_SIZE - LUT_SIZE;
	scratchMem = (uint8_t *)SRAM1_LOC + SRAM1_SIZE - LUT_SIZE - 0x1000;  // 4K should be enough for scratch mem (320/3+2)*8 + 320*8 = 3424
	mem = (uint8_t *)SRAM1_LOC;
	memSize = (uint32_t)scratchMem-SRAM1_LOC;

	len = Chirp::serialize(chirp, mem, memSize,  HTYPE(0), HINT8(0), UINT16(0), UINT16(0), UINTS8_NO_COPY(0), END);
	g_qqueue->flush();
	result = cc_getRLSFrame(scratchMem, lut);
	memSize -= len;
	memSize /= sizeof(Qval);
	// copy from IPC memory to RLS_MEMORY
	numq = g_qqueue->readAll((Qval *)(mem+len), memSize);
	g_chirpM0->service();
	Chirp::serialize(chirp, mem, memSize,  HTYPE(FOURCC('C','C','Q','2')), HINT8(renderFlags), UINT16(CAM_RES2_WIDTH), UINT16(CAM_RES2_HEIGHT), UINTS8_NO_COPY(numq*sizeof(Qval)), END);
	// send frame, use in-place buffer
	chirp->useBuffer((uint8_t *)mem, len+numq*sizeof(Qval));

#endif

	return result;
}

int32_t cc_getRLSFrame(uint8_t *memory, uint8_t *lut, bool sync)
{
	int32_t res;
	int32_t responseInt = -1;

	// check mode, set if necessary
	if ((res=cam_setMode(CAM_MODE1))<0)
		return res;

	// forward call to M0, get frame
	if (sync)
	{
		g_chirpM0->callSync(g_getRLSFrameM0, 
			UINT32((uint32_t)memory), UINT32((uint32_t)lut), END_OUT_ARGS,
			&responseInt, END_IN_ARGS);
		return responseInt;
	}
	else
	{
		g_chirpM0->callAsync(g_getRLSFrameM0, 
			UINT32((uint32_t)memory), UINT32((uint32_t)lut), END_OUT_ARGS);
		return 0;
	}

}

int32_t cc_setMemory(const uint32_t &location, const uint32_t &len, const uint8_t *data)
{
	uint32_t i;
	uint8_t *dest = (uint8_t *)location;
	for (i=0; i<len; i++)
		dest[i] = data[i];

	return len;
}

int cc_sendBlobs(Chirp *chirp, const BlobA *blobs, uint32_t len, uint8_t renderFlags)
{
	CRP_RETURN(chirp, HTYPE(FOURCC('C','C','B','1')), HINT8(renderFlags), HINT16(CAM_RES2_WIDTH), HINT16(CAM_RES2_HEIGHT), UINTS16(len*sizeof(BlobA)/sizeof(uint16_t), blobs), END);
	return 0;
}

int cc_sendBlobs(Chirp *chirp, const BlobA *blobs, uint32_t len, const BlobB *ccBlobs, uint32_t ccLen, uint8_t renderFlags)
{
	CRP_RETURN(chirp, HTYPE(FOURCC('C','C','B','2')), HINT8(renderFlags), HINT16(CAM_RES2_WIDTH), HINT16(CAM_RES2_HEIGHT), UINTS16(len*sizeof(BlobA)/sizeof(uint16_t), blobs), UINTS16(ccLen*sizeof(BlobB)/sizeof(uint16_t), ccBlobs), END);
	return 0;
}

uint8_t ledBrightness(uint32_t area)
{
	uint32_t brightness;

	brightness = 0x100*area/20000;
	if (brightness==0) // can't take log of 0...
		return 1;
	
	// put on log curve
	brightness = log((float)brightness)*50;
	// saturate
	if (brightness>0xff)
		brightness = 0xff;
	else if (brightness==0) 
		brightness = 1;

	return brightness;
}

void cc_setLED()
{
	BlobA *blob;
	uint32_t area, color, r, g, b;
	uint8_t brightness;

	blob = (BlobA *)g_blobs->getMaxBlob();
	if (blob)
	{
		if (blob->m_model<=CL_NUM_SIGNATURES)
			color = g_colors[blob->m_model];
		else
			color = g_colors[0];

		area = (blob->m_right - blob->m_left)*(blob->m_bottom - blob->m_top);
		brightness = ledBrightness(area);
		b = color&0xff;
		b = b ? (b*brightness>>8)+1 : 0;
		color >>= 8;
		g = color&0xff;
		g = g ? (g*brightness>>8)+1 : 0;
		color >>= 8;
		r = color&0xff;
		r = r ? (r*brightness>>8)+1 : 0;
		led_setRGB(r, g, b);
	}
	else
		led_set(0);
}

void cc_sendPoints(Points &points, uint16_t width, uint16_t height, Chirp *chirp, uint8_t renderFlags)
{
	uint32_t len;
	uint8_t *mem = (uint8_t *)SRAM1_LOC;

	len = Chirp::serialize(chirp, mem, SRAM1_SIZE,  HTYPE(0), HINT8(0), UINT16(0), UINT16(0), UINT16(0), UINT16(0), UINTS16_NO_COPY(0), END);

	// copy into video memory because we don't have enough memory in the chirp buffer
	memcpy(mem+len, points.data(), points.size()*sizeof(Point16));
	Chirp::serialize(chirp, mem, SRAM1_SIZE, HTYPE(FOURCC('B','L','T','1')), HINT8(renderFlags), UINT16(CAM_RES2_WIDTH), UINT16(CAM_RES2_HEIGHT), UINT16(width), UINT16(height), UINTS16_NO_COPY(points.size()*sizeof(Point16)/sizeof(uint16_t))); 
	chirp->useBuffer((uint8_t *)mem, len+points.size()*sizeof(Point16));
}

	

