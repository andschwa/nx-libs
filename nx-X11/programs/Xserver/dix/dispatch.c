/************************************************************

Copyright 1987, 1989, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall not be
used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization from The Open Group.


Copyright 1987, 1989 by Digital Equipment Corporation, Maynard, Massachusetts.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its 
documentation for any purpose and without fee is hereby granted, 
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in 
supporting documentation, and that the name of Digital not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.  

DIGITAL DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
DIGITAL BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

********************************************************/

/* The panoramix components contained the following notice */
/*****************************************************************

Copyright (c) 1991, 1997 Digital Equipment Corporation, Maynard, Massachusetts.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
DIGITAL EQUIPMENT CORPORATION BE LIABLE FOR ANY CLAIM, DAMAGES, INCLUDING,
BUT NOT LIMITED TO CONSEQUENTIAL OR INCIDENTAL DAMAGES, OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of Digital Equipment Corporation
shall not be used in advertising or otherwise to promote the sale, use or other
dealings in this Software without prior written authorization from Digital
Equipment Corporation.

******************************************************************/


#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#ifdef PANORAMIX_DEBUG
#include <stdio.h>
int ProcInitialConnection();
#endif

#include "windowstr.h"
#include <X11/fonts/fontstruct.h>
#ifdef HAS_XFONT2
# include <X11/fonts/libxfont2.h>
#else
# include <X11/fonts/fontutil.h>
#endif /* HAS_XFONT2 */
#include "dixfontstr.h"
#include "gcstruct.h"
#include "selection.h"
#include "colormapst.h"
#include "cursorstr.h"
#include "scrnintstr.h"
#include "opaque.h"
#include "input.h"
#include "servermd.h"
#include "extnsionst.h"
#include "dixfont.h"
#include "dispatch.h"
#include "swaprep.h"
#include "swapreq.h"
#ifdef PANORAMIX
#include "../Xext/panoramiX.h"
#include "../Xext/panoramiXsrv.h"
#endif
#ifdef XCSECURITY
#define _SECURITY_SERVER
#include <nx-X11/extensions/security.h>
#endif
#ifdef XKB
#include "inputstr.h"
#include "xkbsrv.h"
#endif
#include "client.h"

#define mskcnt ((MAXCLIENTS + 31) / 32)
#define BITMASK(i) (1U << ((i) & 31))
#define MASKIDX(i) ((i) >> 5)
#define MASKWORD(buf, i) buf[MASKIDX(i)]
#define BITSET(buf, i) MASKWORD(buf, i) |= BITMASK(i)
#define BITCLEAR(buf, i) MASKWORD(buf, i) &= ~BITMASK(i)
#define GETBIT(buf, i) (MASKWORD(buf, i) & BITMASK(i))

extern xConnSetupPrefix connSetupPrefix;
extern char *ConnectionInfo;

extern int screenPrivateCount;

Selection *CurrentSelections;
int NumCurrentSelections;
CallbackListPtr SelectionCallback = NULL;

static ClientPtr grabClient;
#define GrabNone 0
#define GrabActive 1
#define GrabKickout 2
static int grabState = GrabNone;
static long grabWaiters[mskcnt];
CallbackListPtr ServerGrabCallback = NULL;
HWEventQueuePtr checkForInput[2];
extern int connBlockScreenStart;

static void KillAllClients(void);

static void DeleteClientFromAnySelections(ClientPtr client);

static int nextFreeClientID; /* always MIN free client ID */

static int	nClients;	/* number of authorized clients */

CallbackListPtr ClientStateCallback;

/* dispatchException & isItTimeToYield must be declared volatile since they
 * are modified by signal handlers - otherwise optimizer may assume it doesn't
 * need to actually check value in memory when used and may miss changes from
 * signal handlers.
 */
volatile char dispatchException = 0;
volatile char isItTimeToYield;

/* Various of the DIX function interfaces were not designed to allow
 * the client->errorValue to be set on BadValue and other errors.
 * Rather than changing interfaces and breaking untold code we introduce
 * a new global that dispatch can use.
 */
XID clientErrorValue;   /* XXX this is a kludge */

#define SAME_SCREENS(a, b) (\
    (a.pScreen == b.pScreen))

void
SetInputCheck(HWEventQueuePtr c0, HWEventQueuePtr c1)
{
    checkForInput[0] = c0;
    checkForInput[1] = c1;
}

void
UpdateCurrentTime()
{
    TimeStamp systime;

    /* To avoid time running backwards, we must call GetTimeInMillis before
     * calling ProcessInputEvents.
     */
    systime.months = currentTime.months;
    systime.milliseconds = GetTimeInMillis();
    if (systime.milliseconds < currentTime.milliseconds)
	systime.months++;
    if (*checkForInput[0] != *checkForInput[1])
	ProcessInputEvents();
    if (CompareTimeStamps(systime, currentTime) == LATER)
	currentTime = systime;
}

/* Like UpdateCurrentTime, but can't call ProcessInputEvents */
void
UpdateCurrentTimeIf()
{
    TimeStamp systime;

    systime.months = currentTime.months;
    systime.milliseconds = GetTimeInMillis();
    if (systime.milliseconds < currentTime.milliseconds)
	systime.months++;
    if (*checkForInput[0] == *checkForInput[1])
	currentTime = systime;
}

#ifndef NXAGENT_SERVER
void
InitSelections()
{
    if (CurrentSelections)
	free(CurrentSelections);
    CurrentSelections = (Selection *)NULL;
    NumCurrentSelections = 0;
}
#endif /* NXAGENT_SERVER */

#undef SMART_DEBUG

#define SMART_SCHEDULE_DEFAULT_INTERVAL	20	    /* ms */
#define SMART_SCHEDULE_MAX_SLICE	200	    /* ms */

#ifdef HAVE_SETITIMER
#define SMART_SCHEDULE_DEFAULT_SIGNAL_ENABLE HAVE_SETITIMER
Bool SmartScheduleSignalEnable = SMART_SCHEDULE_DEFAULT_SIGNAL_ENABLE;
#endif

long	    SmartScheduleSlice = SMART_SCHEDULE_DEFAULT_INTERVAL;
long	    SmartScheduleInterval = SMART_SCHEDULE_DEFAULT_INTERVAL;
long	    SmartScheduleMaxSlice = SMART_SCHEDULE_MAX_SLICE;
long	    SmartScheduleTime;
ClientPtr   SmartLastClient;
int	    SmartLastIndex[SMART_MAX_PRIORITY-SMART_MIN_PRIORITY+1];
int         SmartScheduleClient(int *clientReady, int nready);

#ifdef SMART_DEBUG
long	    SmartLastPrint;
#endif

void        Dispatch(void);
void        InitProcVectors(void);

int
SmartScheduleClient (int *clientReady, int nready)
{
    int		i;
    int		client;
    ClientPtr	pClient, best = NULL;
    int		bestRobin, robin;
    long	now = SmartScheduleTime;
    long	idle;

    bestRobin = 0;
    idle = 2 * SmartScheduleSlice;
    for (i = 0; i < nready; i++)
    {
	client = clientReady[i];
	pClient = clients[client];
	/* Praise clients which are idle */
	if ((now - pClient->smart_check_tick) >= idle)
	{
	    if (pClient->smart_priority < 0)
		pClient->smart_priority++;
	}
	pClient->smart_check_tick = now;
	
	/* check priority to select best client */
	robin = (pClient->index -
	         SmartLastIndex[pClient->smart_priority -
                                SMART_MIN_PRIORITY]) & 0xff;

	/* pick the best client */
	if (!best ||
	    pClient->priority > best->priority ||
	    (pClient->priority == best->priority &&
	     (pClient->smart_priority > best->smart_priority ||
	      (pClient->smart_priority == best->smart_priority && robin > bestRobin))))
	{
	    best = pClient;
	    bestRobin = robin;
	}
#ifdef SMART_DEBUG
	if ((now - SmartLastPrint) >= 5000)
	    fprintf (stderr, " %2d: %3d", client, pClient->smart_priority);
#endif
    }
#ifdef SMART_DEBUG
    if ((now - SmartLastPrint) >= 5000)
    {
	fprintf (stderr, " use %2d\n", best);
	SmartLastPrint = now;
    }
#endif
    SmartLastIndex[best->smart_priority - SMART_MIN_PRIORITY] = best->index;
    /*
     * Set current client pointer
     */
    if (SmartLastClient != pClient)
    {
	pClient->smart_start_tick = now;
	SmartLastClient = pClient;
    }
    /*
     * Adjust slice
     */
    if (nready == 1)
    {
	/*
	 * If it's been a long time since another client
	 * has run, bump the slice up to get maximal
	 * performance from a single client
	 */
	if ((now - pClient->smart_start_tick) > 1000 &&
	    SmartScheduleSlice < SmartScheduleMaxSlice)
	{
	    SmartScheduleSlice += SmartScheduleInterval;
	}
    }
    else
    {
	SmartScheduleSlice = SmartScheduleInterval;
    }
    return best->index;
}

#ifndef NXAGENT_SERVER
#define MAJOROP ((xReq *)client->requestBuffer)->reqType

void
Dispatch(void)
{
    register int        *clientReady;     /* array of request ready clients */
    register int	result;
    register ClientPtr	client;
    register int	nready;
    register HWEventQueuePtr* icheck = checkForInput;
    long			start_tick;

    nextFreeClientID = 1;
    InitSelections();
    nClients = 0;

    clientReady = (int *) malloc(sizeof(int) * MaxClients);
    if (!clientReady)
	return;

    while (!dispatchException)
    {
        if (*icheck[0] != *icheck[1])
	{
	    ProcessInputEvents();
	    FlushIfCriticalOutputPending();
	}

	nready = WaitForSomething(clientReady);

	if (nready)
	{
	    clientReady[0] = SmartScheduleClient (clientReady, nready);
	    nready = 1;
	}
       /***************** 
	*  Handle events in round robin fashion, doing input between 
	*  each round 
	*****************/

	while (!dispatchException && (--nready >= 0))
	{
	    client = clients[clientReady[nready]];
	    if (! client)
	    {
		/* KillClient can cause this to happen */
		continue;
	    }
	    /* GrabServer activation can cause this to be true */
	    if (grabState == GrabKickout)
	    {
		grabState = GrabActive;
		break;
	    }
	    isItTimeToYield = FALSE;
 
            requestingClient = client;
	    start_tick = SmartScheduleTime;
	    while (!isItTimeToYield)
	    {
	        if (*icheck[0] != *icheck[1])
		{
		    ProcessInputEvents();
		    FlushIfCriticalOutputPending();
		}
		if ((SmartScheduleTime - start_tick) >= SmartScheduleSlice)
		{
		    /* Penalize clients which consume ticks */
		    if (client->smart_priority > SMART_MIN_PRIORITY)
			client->smart_priority--;
		    break;
		}
		/* now, finally, deal with client requests */

	        result = ReadRequestFromClient(client);
	        if (result <= 0) 
	        {
		    if (result < 0)
			CloseDownClient(client);
		    break;
	        }

		client->sequence++;
#ifdef DEBUG
		if (client->requestLogIndex == MAX_REQUEST_LOG)
		    client->requestLogIndex = 0;
		client->requestLog[client->requestLogIndex] = MAJOROP;
		client->requestLogIndex++;
#endif
		if (result > (maxBigRequestSize << 2))
		    result = BadLength;
		else
		    result = (* client->requestVector[MAJOROP])(client);

		if (!SmartScheduleSignalEnable)
		    SmartScheduleTime = GetTimeInMillis();

		if (result != Success) 
		{
		    if (client->noClientException != Success)
                        CloseDownClient(client);
                    else
		        SendErrorToClient(client, MAJOROP,
					  MinorOpcodeOfRequest(client),
					  client->errorValue, result);
		    break;
	        }
#ifdef DAMAGEEXT
		FlushIfCriticalOutputPending ();
#endif
	    }
	    FlushAllOutput();
	    client = clients[clientReady[nready]];
	    if (client)
		client->smart_stop_tick = SmartScheduleTime;
	    requestingClient = NULL;
	}
	dispatchException &= ~DE_PRIORITYCHANGE;
    }
#if defined(DDXBEFORERESET)
    ddxBeforeReset ();
#endif
    KillAllClients();
    free(clientReady);
    dispatchException &= ~DE_RESET;
}

#undef MAJOROP
#endif /* NXAGENT_SERVER */

int
ProcBadRequest(ClientPtr client)
{
    return (BadRequest);
}

int
ProcCreateWindow(ClientPtr client)
{
    register WindowPtr pParent, pWin;
    REQUEST(xCreateWindowReq);
    int result;
    int len;

    REQUEST_AT_LEAST_SIZE(xCreateWindowReq);
    
    LEGAL_NEW_RESOURCE(stuff->wid, client);
    if (!(pParent = (WindowPtr)SecurityLookupWindow(stuff->parent, client,
						    DixWriteAccess)))
        return BadWindow;
    len = client->req_len - (sizeof(xCreateWindowReq) >> 2);
    if (Ones(stuff->mask) != len)
        return BadLength;
    if (!stuff->width || !stuff->height)
    {
	client->errorValue = 0;
        return BadValue;
    }
    pWin = CreateWindow(stuff->wid, pParent, stuff->x,
			      stuff->y, stuff->width, stuff->height, 
			      stuff->borderWidth, stuff->class,
			      stuff->mask, (XID *) &stuff[1], 
			      (int)stuff->depth, 
			      client, stuff->visual, &result);
    if (pWin)
    {
	Mask mask = pWin->eventMask;

	pWin->eventMask = 0; /* subterfuge in case AddResource fails */
	if (!AddResource(stuff->wid, RT_WINDOW, (void *)pWin))
	    return BadAlloc;
	pWin->eventMask = mask;
    }
    if (client->noClientException != Success)
        return(client->noClientException);
    else
        return(result);
}

int
ProcChangeWindowAttributes(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xChangeWindowAttributesReq);
    register int result;
    int len;

    REQUEST_AT_LEAST_SIZE(xChangeWindowAttributesReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->window, client,
					   DixWriteAccess);
    if (!pWin)
        return(BadWindow);
    len = client->req_len - (sizeof(xChangeWindowAttributesReq) >> 2);
    if (len != Ones(stuff->valueMask))
        return BadLength;
    result =  ChangeWindowAttributes(pWin, 
				  stuff->valueMask, 
				  (XID *) &stuff[1], 
				  client);
    if (client->noClientException != Success)
        return(client->noClientException);
    else
        return(result);
}

int
ProcGetWindowAttributes(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xResourceReq);
    xGetWindowAttributesReply wa;

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->id, client,
					   DixReadAccess);
    if (!pWin)
        return(BadWindow);
    memset(&wa, 0, sizeof(xGetWindowAttributesReply));
    GetWindowAttributes(pWin, client, &wa);
    WriteReplyToClient(client, sizeof(xGetWindowAttributesReply), &wa);
    return(client->noClientException);
}

int
ProcDestroyWindow(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->id, client,
					   DixDestroyAccess);
    if (!pWin)
        return(BadWindow);
    if (pWin->parent)
	FreeResource(stuff->id, RT_NONE);
    return(client->noClientException);
}

int
ProcDestroySubwindows(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->id, client,
					   DixDestroyAccess);
    if (!pWin)
        return(BadWindow);
    DestroySubwindows(pWin, client);
    return(client->noClientException);
}

int
ProcChangeSaveSet(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xChangeSaveSetReq);
    register int result;
		  
    REQUEST_SIZE_MATCH(xChangeSaveSetReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->window, client,
					   DixReadAccess);
    if (!pWin)
        return(BadWindow);
    if (client->clientAsMask == (CLIENT_BITS(pWin->drawable.id)))
        return BadMatch;
    if ((stuff->mode == SetModeInsert) || (stuff->mode == SetModeDelete))
    {
        result = AlterSaveSetForClient(client, pWin, stuff->mode, FALSE, TRUE);
	if (client->noClientException != Success)
	    return(client->noClientException);
	else
            return(result);
    }
    else
    {
	client->errorValue = stuff->mode;
	return( BadValue );
    }
}

#ifndef NXAGENT_SERVER
int
ProcReparentWindow(register ClientPtr client)
{
    register WindowPtr pWin, pParent;
    REQUEST(xReparentWindowReq);
    register int result;

    REQUEST_SIZE_MATCH(xReparentWindowReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->window, client,
					   DixWriteAccess);
    if (!pWin)
        return(BadWindow);
    pParent = (WindowPtr)SecurityLookupWindow(stuff->parent, client,
					      DixWriteAccess);
    if (!pParent)
        return(BadWindow);
    if (SAME_SCREENS(pWin->drawable, pParent->drawable))
    {
        if ((pWin->backgroundState == ParentRelative) &&
            (pParent->drawable.depth != pWin->drawable.depth))
            return BadMatch;
	if ((pWin->drawable.class != InputOnly) &&
	    (pParent->drawable.class == InputOnly))
	    return BadMatch;
        result =  ReparentWindow(pWin, pParent, 
			 (short)stuff->x, (short)stuff->y, client);
	if (client->noClientException != Success)
            return(client->noClientException);
	else
            return(result);
    }
    else 
        return (BadMatch);
}
#endif /* NXAGENT_SERVER */

int
ProcMapWindow(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->id, client,
					   DixReadAccess);
    if (!pWin)
        return(BadWindow);
    MapWindow(pWin, client);
           /* update cache to say it is mapped */
    return(client->noClientException);
}

int
ProcMapSubwindows(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow( stuff->id, client,
					    DixReadAccess);
    if (!pWin)
        return(BadWindow);
    MapSubwindows(pWin, client);
           /* update cache to say it is mapped */
    return(client->noClientException);
}

int
ProcUnmapWindow(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow( stuff->id, client,
					    DixReadAccess);
    if (!pWin)
        return(BadWindow);
    UnmapWindow(pWin, FALSE);
           /* update cache to say it is mapped */
    return(client->noClientException);
}

int
ProcUnmapSubwindows(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow( stuff->id, client,
					    DixReadAccess);
    if (!pWin)
        return(BadWindow);
    UnmapSubwindows(pWin);
    return(client->noClientException);
}

int
ProcConfigureWindow(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xConfigureWindowReq);
    register int result;
    int len;

    REQUEST_AT_LEAST_SIZE(xConfigureWindowReq);
    pWin = (WindowPtr)SecurityLookupWindow( stuff->window, client,
					    DixWriteAccess);
    if (!pWin)
        return(BadWindow);
    len = client->req_len - (sizeof(xConfigureWindowReq) >> 2);
    if (Ones((Mask)stuff->mask) != len)
        return BadLength;
    result =  ConfigureWindow(pWin, (Mask)stuff->mask, (XID *) &stuff[1], 
			      client);
    if (client->noClientException != Success)
        return(client->noClientException);
    else
        return(result);
}

int
ProcCirculateWindow(register ClientPtr client)
{
    register WindowPtr pWin;
    REQUEST(xCirculateWindowReq);

    REQUEST_SIZE_MATCH(xCirculateWindowReq);
    if ((stuff->direction != RaiseLowest) &&
	(stuff->direction != LowerHighest))
    {
	client->errorValue = stuff->direction;
        return BadValue;
    }
    pWin = (WindowPtr)SecurityLookupWindow(stuff->window, client,
					   DixWriteAccess);
    if (!pWin)
        return(BadWindow);
    CirculateWindow(pWin, (int)stuff->direction, client);
    return(client->noClientException);
}

int
GetGeometry(register ClientPtr client, xGetGeometryReply *rep)
{
    register DrawablePtr pDraw;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    SECURITY_VERIFY_GEOMETRABLE (pDraw, stuff->id, client, DixReadAccess);
    memset(rep, 0, sizeof(xGetGeometryReply));
    rep->type = X_Reply;
    rep->length = 0;
    rep->sequenceNumber = client->sequence;
    rep->root = pDraw->pScreen->root->drawable.id;
    rep->depth = pDraw->depth;
    rep->width = pDraw->width;
    rep->height = pDraw->height;

    /* XXX - Because the pixmap-implementation of the multibuffer extension 
     *       may have the buffer-id's drawable resource value be a pointer
     *       to the buffer's window instead of the buffer itself
     *       (this happens if the buffer is the displayed buffer),
     *       we also have to check that the id matches before we can
     *       truly say that it is a DRAWABLE_WINDOW.
     */

    if ((pDraw->type == UNDRAWABLE_WINDOW) ||
        ((pDraw->type == DRAWABLE_WINDOW) && (stuff->id == pDraw->id)))
    {
        register WindowPtr pWin = (WindowPtr)pDraw;
	rep->x = pWin->origin.x - wBorderWidth (pWin);
	rep->y = pWin->origin.y - wBorderWidth (pWin);
	rep->borderWidth = pWin->borderWidth;
    }
    else /* DRAWABLE_PIXMAP or DRAWABLE_BUFFER */
    {
	rep->x = rep->y = rep->borderWidth = 0;
    }

    return Success;
}


int
ProcGetGeometry(register ClientPtr client)
{
    xGetGeometryReply rep = {0};
    int status;

    if ((status = GetGeometry(client, &rep)) != Success)
	return status;

    WriteReplyToClient(client, sizeof(xGetGeometryReply), &rep);
    return(client->noClientException);
}


#ifndef NXAGENT_SERVER
int
ProcQueryTree(register ClientPtr client)
{
    xQueryTreeReply reply = {0};
    int numChildren = 0;
    register WindowPtr pChild, pWin, pHead;
    Window  *childIDs = (Window *)NULL;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->id, client,
					   DixReadAccess);
    if (!pWin)
        return(BadWindow);
    reply.type = X_Reply;
    reply.root = pWin->drawable.pScreen->root->drawable.id;
    reply.sequenceNumber = client->sequence;
    if (pWin->parent)
	reply.parent = pWin->parent->drawable.id;
    else
        reply.parent = (Window)None;
    pHead = RealChildHead(pWin);
    for (pChild = pWin->lastChild; pChild != pHead; pChild = pChild->prevSib)
	numChildren++;
    if (numChildren)
    {
	int curChild = 0;

	childIDs = (Window *) malloc(numChildren * sizeof(Window));
	if (!childIDs)
	    return BadAlloc;
	for (pChild = pWin->lastChild; pChild != pHead; pChild = pChild->prevSib)
	    childIDs[curChild++] = pChild->drawable.id;
    }
    
    reply.nChildren = numChildren;
    reply.length = (numChildren * sizeof(Window)) >> 2;
    
    WriteReplyToClient(client, sizeof(xQueryTreeReply), &reply);
    if (numChildren)
    {
    	client->pSwapReplyFunc = (ReplySwapPtr) Swap32Write;
	WriteSwappedDataToClient(client, numChildren * sizeof(Window), childIDs);
	free(childIDs);
    }

    return(client->noClientException);
}
#endif /* NXAGENT_SERVER */


int
ProcInternAtom(register ClientPtr client)
{
    Atom atom;
    char *tchar;
    REQUEST(xInternAtomReq);

    REQUEST_FIXED_SIZE(xInternAtomReq, stuff->nbytes);
    if ((stuff->onlyIfExists != xTrue) && (stuff->onlyIfExists != xFalse))
    {
	client->errorValue = stuff->onlyIfExists;
        return(BadValue);
    }
    tchar = (char *) &stuff[1];
    atom = MakeAtom(tchar, stuff->nbytes, !stuff->onlyIfExists);
    if (atom != BAD_RESOURCE)
    {
	xInternAtomReply reply = {0};
	reply.type = X_Reply;
	reply.length = 0;
	reply.sequenceNumber = client->sequence;
	reply.atom = atom;
	WriteReplyToClient(client, sizeof(xInternAtomReply), &reply);
	return(client->noClientException);
    }
    else
	return (BadAlloc);
}

int
ProcGetAtomName(register ClientPtr client)
{
    const char *str;
    int len;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    if ( (str = NameForAtom(stuff->id)) )
    {
	xGetAtomNameReply reply = {0};
	len = strlen(str);
	reply.type = X_Reply;
	reply.length = (len + 3) >> 2;
	reply.sequenceNumber = client->sequence;
	reply.nameLength = len;
	WriteReplyToClient(client, sizeof(xGetAtomNameReply), &reply);
	WriteToClient(client, len, str);
	return(client->noClientException);
    }
    else 
    { 
	client->errorValue = stuff->id;
	return (BadAtom);
    }
}

#ifndef NXAGENT_SERVER
int
ProcSetSelectionOwner(register ClientPtr client)
{
    WindowPtr pWin;
    TimeStamp time;
    REQUEST(xSetSelectionOwnerReq);

    REQUEST_SIZE_MATCH(xSetSelectionOwnerReq);
    UpdateCurrentTime();
    time = ClientTimeToServerTime(stuff->time);

    /* If the client's time stamp is in the future relative to the server's
	time stamp, do not set the selection, just return success. */
    if (CompareTimeStamps(time, currentTime) == LATER)
    	return Success;
    if (stuff->window != None)
    {
        pWin = (WindowPtr)SecurityLookupWindow(stuff->window, client,
					       DixReadAccess);
        if (!pWin)
            return(BadWindow);
    }
    else
        pWin = (WindowPtr)None;
    if (ValidAtom(stuff->selection))
    {
	int i = 0;

	/*
	 * First, see if the selection is already set... 
	 */
	while ((i < NumCurrentSelections) && 
	       CurrentSelections[i].selection != stuff->selection) 
            i++;
        if (i < NumCurrentSelections)
        {        
	    /* If the timestamp in client's request is in the past relative
		to the time stamp indicating the last time the owner of the
		selection was set, do not set the selection, just return 
		success. */
            if (CompareTimeStamps(time, CurrentSelections[i].lastTimeChanged)
		== EARLIER)
		return Success;
	    if (CurrentSelections[i].client &&
		(!pWin || (CurrentSelections[i].client != client)))
	    {
		xEvent event = {0};
		event.u.u.type = SelectionClear;
		event.u.selectionClear.time = time.milliseconds;
		event.u.selectionClear.window = CurrentSelections[i].window;
		event.u.selectionClear.atom = CurrentSelections[i].selection;
		(void) TryClientEvents (CurrentSelections[i].client, &event, 1,
				NoEventMask, NoEventMask /* CantBeFiltered */,
				NullGrab);
	    }
	}
	else
	{
	    /*
	     * It doesn't exist, so add it...
	     */
	    Selection *newsels;

	    if (i == 0)
		newsels = (Selection *)malloc(sizeof(Selection));
	    else
		newsels = (Selection *)realloc(CurrentSelections,
			    (NumCurrentSelections + 1) * sizeof(Selection));
	    if (!newsels)
		return BadAlloc;
	    NumCurrentSelections++;
	    CurrentSelections = newsels;
	    CurrentSelections[i].selection = stuff->selection;
	}
        CurrentSelections[i].lastTimeChanged = time;
	CurrentSelections[i].window = stuff->window;
	CurrentSelections[i].pWin = pWin;
	CurrentSelections[i].client = (pWin ? client : NullClient);
	if (SelectionCallback)
	{
	    SelectionInfoRec	info = {0};

	    info.selection = &CurrentSelections[i];
	    info.kind= SelectionSetOwner;
	    CallCallbacks(&SelectionCallback, &info);
	}
	return (client->noClientException);
    }
    else 
    {
	client->errorValue = stuff->selection;
        return (BadAtom);
    }
}
#endif /* NXAGENT_SERVER */

int
ProcGetSelectionOwner(register ClientPtr client)
{
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    if (ValidAtom(stuff->id))
    {
	int i;
        xGetSelectionOwnerReply reply;

	i = 0;
        while ((i < NumCurrentSelections) && 
	       CurrentSelections[i].selection != stuff->id) i++;
	memset(&reply, 0, sizeof(xGetSelectionOwnerReply));
        reply.type = X_Reply;
	reply.length = 0;
	reply.sequenceNumber = client->sequence;
        if (i < NumCurrentSelections)
            reply.owner = CurrentSelections[i].window;
        else
            reply.owner = None;
        WriteReplyToClient(client, sizeof(xGetSelectionOwnerReply), &reply);
        return(client->noClientException);
    }
    else            
    {
	client->errorValue = stuff->id;
        return (BadAtom); 
    }
}

#ifndef NXAGENT_SERVER
int
ProcConvertSelection(register ClientPtr client)
{
    Bool paramsOkay;
    xEvent event;
    WindowPtr pWin;
    REQUEST(xConvertSelectionReq);

    REQUEST_SIZE_MATCH(xConvertSelectionReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->requestor, client,
					   DixReadAccess);
    if (!pWin)
        return(BadWindow);

    paramsOkay = (ValidAtom(stuff->selection) && ValidAtom(stuff->target));
    if (stuff->property != None)
	paramsOkay &= ValidAtom(stuff->property);
    if (paramsOkay)
    {
	int i;

	i = 0;
	while ((i < NumCurrentSelections) && 
	       CurrentSelections[i].selection != stuff->selection) i++;
	if ((i < NumCurrentSelections) && 
	    (CurrentSelections[i].window != None)
#ifdef XCSECURITY
	    && (!client->CheckAccess ||
		(* client->CheckAccess)(client, CurrentSelections[i].window,
					RT_WINDOW, DixReadAccess,
					CurrentSelections[i].pWin))
#endif
	    )
	{        
	    memset(&event, 0, sizeof(xEvent));
	    event.u.u.type = SelectionRequest;
	    event.u.selectionRequest.time = stuff->time;
	    event.u.selectionRequest.owner = 
			CurrentSelections[i].window;
	    event.u.selectionRequest.requestor = stuff->requestor;
	    event.u.selectionRequest.selection = stuff->selection;
	    event.u.selectionRequest.target = stuff->target;
	    event.u.selectionRequest.property = stuff->property;
	    if (TryClientEvents(
		CurrentSelections[i].client, &event, 1, NoEventMask,
		NoEventMask /* CantBeFiltered */, NullGrab))
		return (client->noClientException);
	}
	memset(&event, 0, sizeof(xEvent));
	event.u.u.type = SelectionNotify;
	event.u.selectionNotify.time = stuff->time;
	event.u.selectionNotify.requestor = stuff->requestor;
	event.u.selectionNotify.selection = stuff->selection;
	event.u.selectionNotify.target = stuff->target;
	event.u.selectionNotify.property = None;
	(void) TryClientEvents(client, &event, 1, NoEventMask,
			       NoEventMask /* CantBeFiltered */, NullGrab);
	return (client->noClientException);
    }
    else 
    {
	client->errorValue = stuff->property;
        return (BadAtom);
    }
}
#endif /* NXAGENT_SERVER */

int
ProcGrabServer(register ClientPtr client)
{
    REQUEST_SIZE_MATCH(xReq);
    if (grabState != GrabNone && client != grabClient)
    {
	ResetCurrentRequest(client);
	client->sequence--;
	BITSET(grabWaiters, client->index);
	IgnoreClient(client);
	return(client->noClientException);
    }
    OnlyListenToOneClient(client);
    grabState = GrabKickout;
    grabClient = client;

    if (ServerGrabCallback)
    {
	ServerGrabInfoRec grabinfo = {0};
	grabinfo.client = client;
	grabinfo.grabstate  = SERVER_GRABBED;
	CallCallbacks(&ServerGrabCallback, (void *)&grabinfo);
    }

    return(client->noClientException);
}

static void
UngrabServer(ClientPtr client)
{
    int i;

    grabState = GrabNone;
    ListenToAllClients();
    for (i = mskcnt; --i >= 0 && !grabWaiters[i]; )
	;
    if (i >= 0)
    {
	i <<= 5;
	while (!GETBIT(grabWaiters, i))
	    i++;
	BITCLEAR(grabWaiters, i);
	AttendClient(clients[i]);
    }

    if (ServerGrabCallback)
    {
	ServerGrabInfoRec grabinfo = {0};
	grabinfo.client = client;
	grabinfo.grabstate  = SERVER_UNGRABBED;
	CallCallbacks(&ServerGrabCallback, (void *)&grabinfo);
    }
}

int
ProcUngrabServer(register ClientPtr client)
{
    REQUEST_SIZE_MATCH(xReq);
    UngrabServer(client);
    return(client->noClientException);
}

int
ProcTranslateCoords(register ClientPtr client)
{
    REQUEST(xTranslateCoordsReq);

    register WindowPtr pWin, pDst;
    xTranslateCoordsReply rep = {0};

    REQUEST_SIZE_MATCH(xTranslateCoordsReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->srcWid, client,
					   DixReadAccess);
    if (!pWin)
        return(BadWindow);
    pDst = (WindowPtr)SecurityLookupWindow(stuff->dstWid, client,
					   DixReadAccess);
    if (!pDst)
        return(BadWindow);
    rep.type = X_Reply;
    rep.length = 0;
    rep.sequenceNumber = client->sequence;
    if (!SAME_SCREENS(pWin->drawable, pDst->drawable))
    {
	rep.sameScreen = xFalse;
        rep.child = None;
	rep.dstX = rep.dstY = 0;
    }
    else
    {
	INT16 x, y;
	rep.sameScreen = xTrue;
	rep.child = None;
	/* computing absolute coordinates -- adjust to destination later */
	x = pWin->drawable.x + stuff->srcX;
	y = pWin->drawable.y + stuff->srcY;
	pWin = pDst->firstChild;
	while (pWin)
	{
#ifdef SHAPE
	    BoxRec  box = {0};
#endif
	    if ((pWin->mapped) &&
		(x >= pWin->drawable.x - wBorderWidth (pWin)) &&
		(x < pWin->drawable.x + (int)pWin->drawable.width +
		 wBorderWidth (pWin)) &&
		(y >= pWin->drawable.y - wBorderWidth (pWin)) &&
		(y < pWin->drawable.y + (int)pWin->drawable.height +
		 wBorderWidth (pWin))
#ifdef SHAPE
		/* When a window is shaped, a further check
		 * is made to see if the point is inside
		 * borderSize
		 */
		&& (!wBoundingShape(pWin) ||
		    RegionContainsPoint(
					&pWin->borderSize, x, y, &box))
		
		&& (!wInputShape(pWin) ||
		    RegionContainsPoint(
				    wInputShape(pWin),
				    x - pWin->drawable.x,
				    y - pWin->drawable.y, &box))
#endif
		)
            {
		rep.child = pWin->drawable.id;
		pWin = (WindowPtr) NULL;
	    }
	    else
		pWin = pWin->nextSib;
	}
	/* adjust to destination coordinates */
	rep.dstX = x - pDst->drawable.x;
	rep.dstY = y - pDst->drawable.y;
    }
    WriteReplyToClient(client, sizeof(xTranslateCoordsReply), &rep);
    return(client->noClientException);
}

#ifndef NXAGENT_SERVER
int
ProcOpenFont(register ClientPtr client)
{
    int	err;
    REQUEST(xOpenFontReq);

    REQUEST_FIXED_SIZE(xOpenFontReq, stuff->nbytes);
    client->errorValue = stuff->fid;
    LEGAL_NEW_RESOURCE(stuff->fid, client);
    err = OpenFont(client, stuff->fid, (Mask) 0,
		stuff->nbytes, (char *)&stuff[1]);
    if (err == Success)
    {
	return(client->noClientException);
    }
    else
	return err;
}

int
ProcCloseFont(register ClientPtr client)
{
    FontPtr pFont;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pFont = (FontPtr)SecurityLookupIDByType(client, stuff->id, RT_FONT,
					    DixDestroyAccess);
    if ( pFont != (FontPtr)NULL)	/* id was valid */
    {
        FreeResource(stuff->id, RT_NONE);
	return(client->noClientException);
    }
    else
    {
	client->errorValue = stuff->id;
        return (BadFont);
    }
}
#endif /* NXAGENT_SERVER */

int
ProcQueryFont(register ClientPtr client)
{
    xQueryFontReply	*reply;
    FontPtr pFont;
    register GC *pGC;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    client->errorValue = stuff->id;		/* EITHER font or gc */

    pFont = (FontPtr)SecurityLookupIDByType(client, stuff->id, RT_FONT,
					    DixReadAccess);
    if (!pFont)
    {
	  /* can't use VERIFY_GC because it might return BadGC */
	pGC = (GC *) SecurityLookupIDByType(client, stuff->id, RT_GC,
					    DixReadAccess);
        if (!pGC)
	{
	    client->errorValue = stuff->id;
            return(BadFont);     /* procotol spec says only error is BadFont */
	}
	pFont = pGC->font;
    }

    {
	xCharInfo	*pmax = FONTINKMAX(pFont);
	xCharInfo	*pmin = FONTINKMIN(pFont);
	int		nprotoxcistructs;
	int		rlength;

	nprotoxcistructs = (
	   pmax->rightSideBearing == pmin->rightSideBearing &&
	   pmax->leftSideBearing == pmin->leftSideBearing &&
	   pmax->descent == pmin->descent &&
	   pmax->ascent == pmin->ascent &&
	   pmax->characterWidth == pmin->characterWidth) ?
		0 : N2dChars(pFont);

	rlength = sizeof(xQueryFontReply) +
	             FONTINFONPROPS(FONTCHARSET(pFont)) * sizeof(xFontProp)  +
		     nprotoxcistructs * sizeof(xCharInfo);

	reply = (xQueryFontReply *)calloc(1, rlength);
	if(!reply)
	{
	    return(BadAlloc);
	}

	reply->type = X_Reply;
	reply->length = (rlength - sizeof(xGenericReply)) >> 2;
	reply->sequenceNumber = client->sequence;
	QueryFont( pFont, reply, nprotoxcistructs);

        WriteReplyToClient(client, rlength, reply);
	free(reply);
	return(client->noClientException);
    }
}

int
ProcQueryTextExtents(register ClientPtr client)
{
    REQUEST(xQueryTextExtentsReq);
    xQueryTextExtentsReply reply = {0};
    FontPtr pFont;
    GC *pGC;
    ExtentInfoRec info;
    unsigned long length;

    REQUEST_AT_LEAST_SIZE(xQueryTextExtentsReq);
        
    pFont = (FontPtr)SecurityLookupIDByType(client, stuff->fid, RT_FONT,
					    DixReadAccess);
    if (!pFont)
    {
        pGC = (GC *)SecurityLookupIDByType(client, stuff->fid, RT_GC,
					   DixReadAccess);
        if (!pGC)
	{
	    client->errorValue = stuff->fid;
            return(BadFont);
	}
	pFont = pGC->font;
    }
    length = client->req_len - (sizeof(xQueryTextExtentsReq) >> 2);
    length = length << 1;
    if (stuff->oddLength)
    {
	if (length == 0)
	    return(BadLength);
        length--;
    }
#ifdef HAS_XFONT2
    if (!xfont2_query_text_extents(pFont, length, (unsigned char *)&stuff[1], &info))
#else
    if (!QueryTextExtents(pFont, length, (unsigned char *)&stuff[1], &info))
#endif /* HAS_XFONT2 */
	return(BadAlloc);
    reply.type = X_Reply;
    reply.length = 0;
    reply.sequenceNumber = client->sequence;
    reply.drawDirection = info.drawDirection;
    reply.fontAscent = info.fontAscent;
    reply.fontDescent = info.fontDescent;
    reply.overallAscent = info.overallAscent;
    reply.overallDescent = info.overallDescent;
    reply.overallWidth = info.overallWidth;
    reply.overallLeft = info.overallLeft;
    reply.overallRight = info.overallRight;
    WriteReplyToClient(client, sizeof(xQueryTextExtentsReply), &reply);
    return(client->noClientException);
}

#ifndef NXAGENT_SERVER
int
ProcListFonts(register ClientPtr client)
{
    REQUEST(xListFontsReq);

    REQUEST_FIXED_SIZE(xListFontsReq, stuff->nbytes);

    return ListFonts(client, (unsigned char *) &stuff[1], stuff->nbytes, 
	stuff->maxNames);
}

int
ProcListFontsWithInfo(register ClientPtr client)
{
    REQUEST(xListFontsWithInfoReq);

    REQUEST_FIXED_SIZE(xListFontsWithInfoReq, stuff->nbytes);

    return StartListFontsWithInfo(client, stuff->nbytes,
				  (unsigned char *) &stuff[1], stuff->maxNames);
}
#endif /* NXAGENT_SERVER */

/**
 *
 *  \param value must conform to DeleteType
 */
int
dixDestroyPixmap(void * value, XID pid)
{
    PixmapPtr pPixmap = (PixmapPtr)value;
    return (*pPixmap->drawable.pScreen->DestroyPixmap)(pPixmap);
}

int
ProcCreatePixmap(register ClientPtr client)
{
    PixmapPtr pMap;
    register DrawablePtr pDraw;
    REQUEST(xCreatePixmapReq);
    DepthPtr pDepth;
    register int i;

    REQUEST_SIZE_MATCH(xCreatePixmapReq);
    client->errorValue = stuff->pid;
    LEGAL_NEW_RESOURCE(stuff->pid, client);
    SECURITY_VERIFY_GEOMETRABLE (pDraw, stuff->drawable, client,
				 DixReadAccess);
    if (!stuff->width || !stuff->height)
    {
	client->errorValue = 0;
        return BadValue;
    }
    if (stuff->width > 32767 || stuff->height > 32767)
    {
	/* It is allowed to try and allocate a pixmap which is larger than
	 * 32767 in either dimension. However, all of the framebuffer code
	 * is buggy and does not reliably draw to such big pixmaps, basically
	 * because the Region data structure operates with signed shorts
	 * for the rectangles in it.
	 *
	 * Furthermore, several places in the X server computes the
	 * size in bytes of the pixmap and tries to store it in an
	 * integer. This integer can overflow and cause the allocated size
	 * to be much smaller.
	 *
	 * So, such big pixmaps are rejected here with a BadAlloc
	 */
	return BadAlloc;
    }
    if (stuff->depth != 1)
    {
        pDepth = pDraw->pScreen->allowedDepths;
        for (i=0; i<pDraw->pScreen->numDepths; i++, pDepth++)
	   if (pDepth->depth == stuff->depth)
               goto CreatePmap;
	client->errorValue = stuff->depth;
        return BadValue;
    }
CreatePmap:
    pMap = (PixmapPtr)(*pDraw->pScreen->CreatePixmap)
		(pDraw->pScreen, stuff->width,
		 stuff->height, stuff->depth, 0);
    if (pMap)
    {
	pMap->drawable.serialNumber = NEXT_SERIAL_NUMBER;
	pMap->drawable.id = stuff->pid;
	if (AddResource(stuff->pid, RT_PIXMAP, (void *)pMap))
	    return(client->noClientException);
    }
    return (BadAlloc);
}

#ifndef NXAGENT_SERVER
int
ProcFreePixmap(register ClientPtr client)
{
    PixmapPtr pMap;

    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pMap = (PixmapPtr)SecurityLookupIDByType(client, stuff->id, RT_PIXMAP,
					     DixDestroyAccess);
    if (pMap) 
    {
	FreeResource(stuff->id, RT_NONE);
	return(client->noClientException);
    }
    else 
    {
	client->errorValue = stuff->id;
	return (BadPixmap);
    }
}
#endif /* NXAGENT_SERVER */

int
ProcCreateGC(register ClientPtr client)
{
    int error;
    GC *pGC;
    register DrawablePtr pDraw;
    unsigned len;
    REQUEST(xCreateGCReq);

    REQUEST_AT_LEAST_SIZE(xCreateGCReq);
    client->errorValue = stuff->gc;
    LEGAL_NEW_RESOURCE(stuff->gc, client);
    SECURITY_VERIFY_DRAWABLE (pDraw, stuff->drawable, client,
			      DixReadAccess);
    len = client->req_len -  (sizeof(xCreateGCReq) >> 2);
    if (len != Ones(stuff->mask))
        return BadLength;
    pGC = (GC *)CreateGC(pDraw, stuff->mask, 
			 (XID *) &stuff[1], &error);
    if (error != Success)
        return error;
    if (!AddResource(stuff->gc, RT_GC, (void *)pGC))
	return (BadAlloc);
    return(client->noClientException);
}

int
ProcChangeGC(register ClientPtr client)
{
    GC *pGC;
    REQUEST(xChangeGCReq);
    int result;
    unsigned len;
		
    REQUEST_AT_LEAST_SIZE(xChangeGCReq);
    SECURITY_VERIFY_GC(pGC, stuff->gc, client, DixWriteAccess);
    len = client->req_len -  (sizeof(xChangeGCReq) >> 2);
    if (len != Ones(stuff->mask))
        return BadLength;

    result = dixChangeGC(client, pGC, stuff->mask, (CARD32 *) &stuff[1], 0);
    if (client->noClientException != Success)
        return(client->noClientException);
    else
    {
	client->errorValue = clientErrorValue;
        return(result);
    }
}

int
ProcCopyGC(register ClientPtr client)
{
    register GC *dstGC;
    register GC *pGC;
    int result;
    REQUEST(xCopyGCReq);

    REQUEST_SIZE_MATCH(xCopyGCReq);
    SECURITY_VERIFY_GC( pGC, stuff->srcGC, client, DixReadAccess);
    SECURITY_VERIFY_GC( dstGC, stuff->dstGC, client, DixWriteAccess);
    if ((dstGC->pScreen != pGC->pScreen) || (dstGC->depth != pGC->depth))
        return (BadMatch);    
    result = CopyGC(pGC, dstGC, stuff->mask);
    if (client->noClientException != Success)
        return(client->noClientException);
    else
    {
	client->errorValue = clientErrorValue;
        return(result);
    }
}

int
ProcSetDashes(register ClientPtr client)
{
    register GC *pGC;
    int result;
    REQUEST(xSetDashesReq);

    REQUEST_FIXED_SIZE(xSetDashesReq, stuff->nDashes);
    if (stuff->nDashes == 0)
    {
	 client->errorValue = 0;
         return BadValue;
    }

    SECURITY_VERIFY_GC(pGC,stuff->gc, client, DixWriteAccess);

    result = SetDashes(pGC, stuff->dashOffset, stuff->nDashes,
		       (unsigned char *)&stuff[1]);
    if (client->noClientException != Success)
        return(client->noClientException);
    else
    {
	client->errorValue = clientErrorValue;
        return(result);
    }
}

int
ProcSetClipRectangles(register ClientPtr client)
{
    int	nr;
    int result;
    register GC *pGC;
    REQUEST(xSetClipRectanglesReq);

    REQUEST_AT_LEAST_SIZE(xSetClipRectanglesReq);
    if ((stuff->ordering != Unsorted) && (stuff->ordering != YSorted) &&
	(stuff->ordering != YXSorted) && (stuff->ordering != YXBanded))
    {
	client->errorValue = stuff->ordering;
        return BadValue;
    }
    SECURITY_VERIFY_GC(pGC,stuff->gc, client, DixWriteAccess);
		 
    nr = (client->req_len << 2) - sizeof(xSetClipRectanglesReq);
    if (nr & 4)
	return(BadLength);
    nr >>= 3;
    result = SetClipRects(pGC, stuff->xOrigin, stuff->yOrigin,
			  nr, (xRectangle *)&stuff[1], (int)stuff->ordering);
    if (client->noClientException != Success)
        return(client->noClientException);
    else
        return(result);
}

int
ProcFreeGC(register ClientPtr client)
{
    register GC *pGC;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    SECURITY_VERIFY_GC(pGC, stuff->id, client, DixDestroyAccess);
    FreeResource(stuff->id, RT_NONE);
    return(client->noClientException);
}

int
ProcClearToBackground(register ClientPtr client)
{
    REQUEST(xClearAreaReq);
    register WindowPtr pWin;

    REQUEST_SIZE_MATCH(xClearAreaReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->window, client,
					   DixWriteAccess);
    if (!pWin)
        return(BadWindow);
    if (pWin->drawable.class == InputOnly)
    {
	client->errorValue = stuff->window;
	return (BadMatch);
    }		    
    if ((stuff->exposures != xTrue) && (stuff->exposures != xFalse))
    {
	client->errorValue = stuff->exposures;
        return(BadValue);
    }
    (*pWin->drawable.pScreen->ClearToBackground)(pWin, stuff->x, stuff->y,
			       stuff->width, stuff->height,
			       (Bool)stuff->exposures);
    return(client->noClientException);
}

int
ProcCopyArea(register ClientPtr client)
{
    register DrawablePtr pDst;
    register DrawablePtr pSrc;
    register GC *pGC;
    REQUEST(xCopyAreaReq);
    RegionPtr pRgn;

    REQUEST_SIZE_MATCH(xCopyAreaReq);

    VALIDATE_DRAWABLE_AND_GC(stuff->dstDrawable, pDst, pGC, client); 
    if (stuff->dstDrawable != stuff->srcDrawable)
    {
	SECURITY_VERIFY_DRAWABLE(pSrc, stuff->srcDrawable, client,
				 DixReadAccess);
	if ((pDst->pScreen != pSrc->pScreen) || (pDst->depth != pSrc->depth))
	{
	    client->errorValue = stuff->dstDrawable;
	    return (BadMatch);
	}
    }
    else
        pSrc = pDst;

    SET_DBE_SRCBUF(pSrc, stuff->srcDrawable);

    pRgn = (*pGC->ops->CopyArea)(pSrc, pDst, pGC, stuff->srcX, stuff->srcY,
				 stuff->width, stuff->height, 
				 stuff->dstX, stuff->dstY);
    if (pGC->graphicsExposures)
    {
	(*pDst->pScreen->SendGraphicsExpose)
 		(client, pRgn, stuff->dstDrawable, X_CopyArea, 0);
	if (pRgn)
	    RegionDestroy(pRgn);
    }

    return(client->noClientException);
}

int
ProcCopyPlane(register ClientPtr client)
{
    register DrawablePtr psrcDraw, pdstDraw;
    register GC *pGC;
    REQUEST(xCopyPlaneReq);
    RegionPtr pRgn;

    REQUEST_SIZE_MATCH(xCopyPlaneReq);

    VALIDATE_DRAWABLE_AND_GC(stuff->dstDrawable, pdstDraw, pGC, client);
    if (stuff->dstDrawable != stuff->srcDrawable)
    {
	SECURITY_VERIFY_DRAWABLE(psrcDraw, stuff->srcDrawable, client,
				 DixReadAccess);
	if (pdstDraw->pScreen != psrcDraw->pScreen)
	{
	    client->errorValue = stuff->dstDrawable;
	    return (BadMatch);
	}
    }
    else
        psrcDraw = pdstDraw;

    SET_DBE_SRCBUF(psrcDraw, stuff->srcDrawable);

    /* Check to see if stuff->bitPlane has exactly ONE good bit set */
    if(stuff->bitPlane == 0 || (stuff->bitPlane & (stuff->bitPlane - 1)) ||
       (stuff->bitPlane > (1L << (psrcDraw->depth - 1))))
    {
       client->errorValue = stuff->bitPlane;
       return(BadValue);
    }

    pRgn = (*pGC->ops->CopyPlane)(psrcDraw, pdstDraw, pGC, stuff->srcX, stuff->srcY,
				 stuff->width, stuff->height, 
				 stuff->dstX, stuff->dstY, stuff->bitPlane);
    if (pGC->graphicsExposures)
    {
	(*pdstDraw->pScreen->SendGraphicsExpose)
 		(client, pRgn, stuff->dstDrawable, X_CopyPlane, 0);
	if (pRgn)
	    RegionDestroy(pRgn);
    }
    return(client->noClientException);
}

int
ProcPolyPoint(register ClientPtr client)
{
    int npoint;
    register GC *pGC;
    register DrawablePtr pDraw;
    REQUEST(xPolyPointReq);

    REQUEST_AT_LEAST_SIZE(xPolyPointReq);
    if ((stuff->coordMode != CoordModeOrigin) && 
	(stuff->coordMode != CoordModePrevious))
    {
	client->errorValue = stuff->coordMode;
        return BadValue;
    }
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client); 
    npoint = ((client->req_len << 2) - sizeof(xPolyPointReq)) >> 2;
    if (npoint)
        (*pGC->ops->PolyPoint)(pDraw, pGC, stuff->coordMode, npoint,
			  (xPoint *) &stuff[1]);
    return (client->noClientException);
}

int
ProcPolyLine(register ClientPtr client)
{
    int npoint;
    register GC *pGC;
    register DrawablePtr pDraw;
    REQUEST(xPolyLineReq);

    REQUEST_AT_LEAST_SIZE(xPolyLineReq);
    if ((stuff->coordMode != CoordModeOrigin) && 
	(stuff->coordMode != CoordModePrevious))
    {
	client->errorValue = stuff->coordMode;
        return BadValue;
    }
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);
    npoint = ((client->req_len << 2) - sizeof(xPolyLineReq)) >> 2;
    if (npoint > 1)
	(*pGC->ops->Polylines)(pDraw, pGC, stuff->coordMode, npoint, 
			      (DDXPointPtr) &stuff[1]);
    return(client->noClientException);
}

int
ProcPolySegment(register ClientPtr client)
{
    int nsegs;
    register GC *pGC;
    register DrawablePtr pDraw;
    REQUEST(xPolySegmentReq);

    REQUEST_AT_LEAST_SIZE(xPolySegmentReq);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);
    nsegs = (client->req_len << 2) - sizeof(xPolySegmentReq);
    if (nsegs & 4)
	return(BadLength);
    nsegs >>= 3;
    if (nsegs)
        (*pGC->ops->PolySegment)(pDraw, pGC, nsegs, (xSegment *) &stuff[1]);
    return (client->noClientException);
}

int
ProcPolyRectangle (register ClientPtr client)
{
    int nrects;
    register GC *pGC;
    register DrawablePtr pDraw;
    REQUEST(xPolyRectangleReq);

    REQUEST_AT_LEAST_SIZE(xPolyRectangleReq);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);
    nrects = (client->req_len << 2) - sizeof(xPolyRectangleReq);
    if (nrects & 4)
	return(BadLength);
    nrects >>= 3;
    if (nrects)
        (*pGC->ops->PolyRectangle)(pDraw, pGC, 
		    nrects, (xRectangle *) &stuff[1]);
    return(client->noClientException);
}

int
ProcPolyArc(register ClientPtr client)
{
    int		narcs;
    register GC *pGC;
    register DrawablePtr pDraw;
    REQUEST(xPolyArcReq);

    REQUEST_AT_LEAST_SIZE(xPolyArcReq);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);
    narcs = (client->req_len << 2) - sizeof(xPolyArcReq);
    if (narcs % sizeof(xArc))
	return(BadLength);
    narcs /= sizeof(xArc);
    if (narcs)
        (*pGC->ops->PolyArc)(pDraw, pGC, narcs, (xArc *) &stuff[1]);
    return (client->noClientException);
}

int
ProcFillPoly(register ClientPtr client)
{
    int          things;
    register GC *pGC;
    register DrawablePtr pDraw;
    REQUEST(xFillPolyReq);

    REQUEST_AT_LEAST_SIZE(xFillPolyReq);
    if ((stuff->shape != Complex) && (stuff->shape != Nonconvex) &&  
	(stuff->shape != Convex))
    {
	client->errorValue = stuff->shape;
        return BadValue;
    }
    if ((stuff->coordMode != CoordModeOrigin) && 
	(stuff->coordMode != CoordModePrevious))
    {
	client->errorValue = stuff->coordMode;
        return BadValue;
    }

    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);
    things = ((client->req_len << 2) - sizeof(xFillPolyReq)) >> 2;
    if (things)
        (*pGC->ops->FillPolygon) (pDraw, pGC, stuff->shape,
			 stuff->coordMode, things,
			 (DDXPointPtr) &stuff[1]);
    return(client->noClientException);
}

int
ProcPolyFillRectangle(register ClientPtr client)
{
    int             things;
    register GC *pGC;
    register DrawablePtr pDraw;
    REQUEST(xPolyFillRectangleReq);

    REQUEST_AT_LEAST_SIZE(xPolyFillRectangleReq);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);
    things = (client->req_len << 2) - sizeof(xPolyFillRectangleReq);
    if (things & 4)
	return(BadLength);
    things >>= 3;

    if (things)
        (*pGC->ops->PolyFillRect) (pDraw, pGC, things,
		      (xRectangle *) &stuff[1]);
    return (client->noClientException);
}

int
ProcPolyFillArc(register ClientPtr client)
{
    int		narcs;
    register GC *pGC;
    register DrawablePtr pDraw;
    REQUEST(xPolyFillArcReq);

    REQUEST_AT_LEAST_SIZE(xPolyFillArcReq);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);
    narcs = (client->req_len << 2) - sizeof(xPolyFillArcReq);
    if (narcs % sizeof(xArc))
	return(BadLength);
    narcs /= sizeof(xArc);
    if (narcs)
        (*pGC->ops->PolyFillArc) (pDraw, pGC, narcs, (xArc *) &stuff[1]);
    return (client->noClientException);
}

#ifdef MATCH_CLIENT_ENDIAN

int
ServerOrder (void)
{
    int	    whichbyte = 1;

    if (*((char *) &whichbyte))
	return LSBFirst;
    return MSBFirst;
}

#define ClientOrder(client) ((client)->swapped ? !ServerOrder() : ServerOrder())

void
ReformatImage (char *base, int nbytes, int bpp, int order)
{
    switch (bpp) {
    case 1:	/* yuck */
	if (BITMAP_BIT_ORDER != order)
	    BitOrderInvert ((unsigned char *) base, nbytes);
#if IMAGE_BYTE_ORDER != BITMAP_BIT_ORDER && BITMAP_SCANLINE_UNIT != 8
	ReformatImage (base, nbytes, BITMAP_SCANLINE_UNIT, order);
#endif
	break;
    case 4:
	break;  /* yuck */
    case 8:
	break;
    case 16:
	if (IMAGE_BYTE_ORDER != order)
	    TwoByteSwap ((unsigned char *) base, nbytes);
	break;
    case 32:
	if (IMAGE_BYTE_ORDER != order)
	    FourByteSwap ((unsigned char *) base, nbytes);
	break;
    }
}
#else
#define ReformatImage(b,n,bpp,o)
#endif

/* 64-bit server notes: the protocol restricts padding of images to
 * 8-, 16-, or 32-bits. We would like to have 64-bits for the server
 * to use internally. Removes need for internal alignment checking.
 * All of the PutImage functions could be changed individually, but
 * as currently written, they call other routines which require things
 * to be 64-bit padded on scanlines, so we changed things here.
 * If an image would be padded differently for 64- versus 32-, then
 * copy each scanline to a 64-bit padded scanline.
 * Also, we need to make sure that the image is aligned on a 64-bit
 * boundary, even if the scanlines are padded to our satisfaction.
 */
int
ProcPutImage(register ClientPtr client)
{
    register	GC *pGC;
    register	DrawablePtr pDraw;
    long	length; 	/* length of scanline server padded */
    long 	lengthProto; 	/* length of scanline protocol padded */
    char	*tmpImage;
    REQUEST(xPutImageReq);

    REQUEST_AT_LEAST_SIZE(xPutImageReq);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);
    if (stuff->format == XYBitmap)
    {
        if ((stuff->depth != 1) ||
	    (stuff->leftPad >= (unsigned int)screenInfo.bitmapScanlinePad))
            return BadMatch;
        length 	    = BitmapBytePad(stuff->width + stuff->leftPad);
    }
    else if (stuff->format == XYPixmap)
    {
        if ((pDraw->depth != stuff->depth) || 
	    (stuff->leftPad >= (unsigned int)screenInfo.bitmapScanlinePad))
            return BadMatch;
        length      = BitmapBytePad(stuff->width + stuff->leftPad);
	length      *= stuff->depth;
    }
    else if (stuff->format == ZPixmap)
    {
        if ((pDraw->depth != stuff->depth) || (stuff->leftPad != 0))
            return BadMatch;
        length      = PixmapBytePad(stuff->width, stuff->depth);
    }
    else
    {
	client->errorValue = stuff->format;
        return BadValue;
    }

    tmpImage = (char *)&stuff[1];
    lengthProto = length;
    if (stuff->height != 0 && lengthProto >= (INT32_MAX / stuff->height))
        return BadLength;

    if (((((lengthProto * stuff->height) + (unsigned)3) >> 2) + 
	(sizeof(xPutImageReq) >> 2)) != client->req_len)
	return BadLength;

    ReformatImage (tmpImage, lengthProto * stuff->height, 
		   stuff->format == ZPixmap ? BitsPerPixel (stuff->depth) : 1,
		   ClientOrder(client));
    
    (*pGC->ops->PutImage) (pDraw, pGC, stuff->depth, stuff->dstX, stuff->dstY,
		  stuff->width, stuff->height, 
		  stuff->leftPad, stuff->format, tmpImage);

     return (client->noClientException);
}


int
DoGetImage(register ClientPtr client, int format, Drawable drawable, 
           int x, int y, int width, int height, 
           Mask planemask, xGetImageReply **im_return)
{
    register DrawablePtr pDraw;
    int			nlines, linesPerBuf;
    register int	linesDone;
    long		widthBytesLine, length;
    Mask		plane = 0;
    char		*pBuf;
    xGetImageReply	xgi = {0};
#ifdef XCSECURITY
    RegionPtr pVisibleRegion = NULL;
#endif

    if ((format != XYPixmap) && (format != ZPixmap))
    {
	client->errorValue = format;
        return(BadValue);
    }
    SECURITY_VERIFY_DRAWABLE(pDraw, drawable, client, DixReadAccess);

    if(pDraw->type == DRAWABLE_WINDOW)
    {
      if( /* check for being viewable */
	 !((WindowPtr) pDraw)->realized ||
	  /* check for being on screen */
         pDraw->x + x < 0 ||
 	 pDraw->x + x + width > pDraw->pScreen->width ||
         pDraw->y + y < 0 ||
         pDraw->y + y + height > pDraw->pScreen->height ||
          /* check for being inside of border */
         x < - wBorderWidth((WindowPtr)pDraw) ||
         x + width > wBorderWidth((WindowPtr)pDraw) + (int)pDraw->width ||
         y < -wBorderWidth((WindowPtr)pDraw) ||
         y + height > wBorderWidth ((WindowPtr)pDraw) + (int)pDraw->height
        )
	    return(BadMatch);
	xgi.visual = wVisual (((WindowPtr) pDraw));
    }
    else
    {
      if(x < 0 ||
         x+width > (int)pDraw->width ||
         y < 0 ||
         y+height > (int)pDraw->height
        )
	    return(BadMatch);
	xgi.visual = None;
    }

    SET_DBE_SRCBUF(pDraw, drawable);

    xgi.type = X_Reply;
    xgi.sequenceNumber = client->sequence;
    xgi.depth = pDraw->depth;
    if(format == ZPixmap)
    {
	widthBytesLine = PixmapBytePad(width, pDraw->depth);
	length = widthBytesLine * height;

    }
    else 
    {
	widthBytesLine = BitmapBytePad(width);
	plane = ((Mask)1) << (pDraw->depth - 1);
	/* only planes asked for */
	length = widthBytesLine * height *
		 Ones(planemask & (plane | (plane - 1)));

    }

    xgi.length = length;

    if (im_return) {
	pBuf = (char *)calloc(1, sz_xGetImageReply + length);
	if (!pBuf)
	    return (BadAlloc);
	if (widthBytesLine == 0)
	    linesPerBuf = 0;
	else
	    linesPerBuf = height;
	*im_return = (xGetImageReply *)pBuf;
	*(xGetImageReply *)pBuf = xgi;
	pBuf += sz_xGetImageReply;
    } else {
	xgi.length = (xgi.length + 3) >> 2;
	if (widthBytesLine == 0 || height == 0)
	    linesPerBuf = 0;
	else if (widthBytesLine >= IMAGE_BUFSIZE)
	    linesPerBuf = 1;
	else
	{
	    linesPerBuf = IMAGE_BUFSIZE / widthBytesLine;
	    if (linesPerBuf > height)
		linesPerBuf = height;
	}
	length = linesPerBuf * widthBytesLine;
	if (linesPerBuf < height)
	{
	    /* we have to make sure intermediate buffers don't need padding */
	    while ((linesPerBuf > 1) &&
		   (length & ((1L << LOG2_BYTES_PER_SCANLINE_PAD)-1)))
	    {
		linesPerBuf--;
		length -= widthBytesLine;
	    }
	    while (length & ((1L << LOG2_BYTES_PER_SCANLINE_PAD)-1))
	    {
		linesPerBuf++;
		length += widthBytesLine;
	    }
	}
	if(!(pBuf = (char *) malloc(length)))
	    return (BadAlloc);
	memset(pBuf, 0, length);
	WriteReplyToClient(client, sizeof (xGetImageReply), &xgi);
    }

#ifdef XCSECURITY
    if (client->trustLevel != XSecurityClientTrusted &&
	pDraw->type == DRAWABLE_WINDOW)
    {
	pVisibleRegion = NotClippedByChildren((WindowPtr)pDraw);
	if (pVisibleRegion)
	{
	    RegionTranslate(pVisibleRegion, -pDraw->x, -pDraw->y);
	}
    }
#endif

    if (linesPerBuf == 0)
    {
	/* nothing to do */
    }
    else if (format == ZPixmap)
    {
        linesDone = 0;
        while (height - linesDone > 0)
        {
	    nlines = min(linesPerBuf, height - linesDone);
	    (*pDraw->pScreen->GetImage) (pDraw,
	                                 x,
				         y + linesDone,
				         width, 
				         nlines,
				         format,
				         planemask,
				         (void *) pBuf);
#ifdef XCSECURITY
	    if (pVisibleRegion)
		SecurityCensorImage(client, pVisibleRegion, widthBytesLine,
			pDraw, x, y + linesDone, width, 
			nlines, format, pBuf);
#endif

	    /* Note that this is NOT a call to WriteSwappedDataToClient,
               as we do NOT byte swap */
	    if (!im_return)
	    {
		ReformatImage (pBuf, (int)(nlines * widthBytesLine),
			       BitsPerPixel (pDraw->depth),
			       ClientOrder(client));

/* Don't split me, gcc pukes when you do */
		WriteToClient(client,
				    (int)(nlines * widthBytesLine),
				    pBuf);
	    }
	    linesDone += nlines;
        }
    }
    else /* XYPixmap */
    {
        for (; plane; plane >>= 1)
	{
	    if (planemask & plane)
	    {
	        linesDone = 0;
	        while (height - linesDone > 0)
	        {
		    nlines = min(linesPerBuf, height - linesDone);
	            (*pDraw->pScreen->GetImage) (pDraw,
	                                         x,
				                 y + linesDone,
				                 width, 
				                 nlines,
				                 format,
				                 plane,
				                 (void *)pBuf);
#ifdef XCSECURITY
		    if (pVisibleRegion)
			SecurityCensorImage(client, pVisibleRegion,
				widthBytesLine,
				pDraw, x, y + linesDone, width, 
				nlines, format, pBuf);
#endif

		    /* Note: NOT a call to WriteSwappedDataToClient,
		       as we do NOT byte swap */
		    if (im_return) {
			pBuf += nlines * widthBytesLine;
		    } else {
			ReformatImage (pBuf, 
				       (int)(nlines * widthBytesLine), 
				       1,
				       ClientOrder (client));

/* Don't split me, gcc pukes when you do */
			WriteToClient(client,
					(int)(nlines * widthBytesLine),
					pBuf);
		    }
		    linesDone += nlines;
		}
            }
	}
    }
#ifdef XCSECURITY
    if (pVisibleRegion)
	RegionDestroy(pVisibleRegion);
#endif
    if (!im_return)
	free(pBuf);
    return (client->noClientException);
}

int
ProcGetImage(register ClientPtr client)
{
    REQUEST(xGetImageReq);

    REQUEST_SIZE_MATCH(xGetImageReq);

    return DoGetImage(client, stuff->format, stuff->drawable,
		      stuff->x, stuff->y,
		      (int)stuff->width, (int)stuff->height,
		      stuff->planeMask, (xGetImageReply **)NULL);
}

int
ProcPolyText(register ClientPtr client)
{
    int	err;
    REQUEST(xPolyTextReq);
    DrawablePtr pDraw;
    GC *pGC;

    REQUEST_AT_LEAST_SIZE(xPolyTextReq);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);

    err = PolyText(client,
		   pDraw,
		   pGC,
		   (unsigned char *)&stuff[1],
		   ((unsigned char *) stuff) + (client->req_len << 2),
		   stuff->x,
		   stuff->y,
		   stuff->reqType,
		   stuff->drawable);

    if (err == Success)
    {
	return(client->noClientException);
    }
    else
	return err;
}

int
ProcImageText8(register ClientPtr client)
{
    int	err;
    register DrawablePtr pDraw;
    register GC *pGC;

    REQUEST(xImageTextReq);

    REQUEST_FIXED_SIZE(xImageTextReq, stuff->nChars);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);

    err = ImageText(client,
		    pDraw,
		    pGC,
		    stuff->nChars,
		    (unsigned char *)&stuff[1],
		    stuff->x,
		    stuff->y,
		    stuff->reqType,
		    stuff->drawable);

    if (err == Success)
    {
	return(client->noClientException);
    }
    else
	return err;
}

int
ProcImageText16(register ClientPtr client)
{
    int	err;
    register DrawablePtr pDraw;
    register GC *pGC;

    REQUEST(xImageTextReq);

    REQUEST_FIXED_SIZE(xImageTextReq, stuff->nChars << 1);
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, pGC, client);

    err = ImageText(client,
		    pDraw,
		    pGC,
		    stuff->nChars,
		    (unsigned char *)&stuff[1],
		    stuff->x,
		    stuff->y,
		    stuff->reqType,
		    stuff->drawable);

    if (err == Success)
    {
	return(client->noClientException);
    }
    else
	return err;
}


int
ProcCreateColormap(register ClientPtr client)
{
    VisualPtr	pVisual;
    ColormapPtr	pmap;
    Colormap	mid;
    register WindowPtr   pWin;
    ScreenPtr pScreen;
    REQUEST(xCreateColormapReq);
    int i, result;

    REQUEST_SIZE_MATCH(xCreateColormapReq);

    if ((stuff->alloc != AllocNone) && (stuff->alloc != AllocAll))
    {
	client->errorValue = stuff->alloc;
        return(BadValue);
    }
    mid = stuff->mid;
    LEGAL_NEW_RESOURCE(mid, client);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->window, client,
					   DixReadAccess);
    if (!pWin)
        return(BadWindow);

    pScreen = pWin->drawable.pScreen;
    for (i = 0, pVisual = pScreen->visuals;
	 i < pScreen->numVisuals;
	 i++, pVisual++)
    {
	if (pVisual->vid != stuff->visual)
	    continue;
	result =  CreateColormap(mid, pScreen, pVisual, &pmap,
				 (int)stuff->alloc, client->index);
	if (client->noClientException != Success)
	    return(client->noClientException);
	else
	    return(result);
    }
    client->errorValue = stuff->visual;
    return(BadMatch);
}

int
ProcFreeColormap(register ClientPtr client)
{
    ColormapPtr pmap;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pmap = (ColormapPtr )SecurityLookupIDByType(client, stuff->id, RT_COLORMAP,
						DixDestroyAccess);
    if (pmap) 
    {
	/* Freeing a default colormap is a no-op */
	if (!(pmap->flags & IsDefault))
	    FreeResource(stuff->id, RT_NONE);
	return (client->noClientException);
    }
    else 
    {
	client->errorValue = stuff->id;
	return (BadColor);
    }
}


int
ProcCopyColormapAndFree(register ClientPtr client)
{
    Colormap	mid;
    ColormapPtr	pSrcMap;
    REQUEST(xCopyColormapAndFreeReq);
    int result;

    REQUEST_SIZE_MATCH(xCopyColormapAndFreeReq);
    mid = stuff->mid;
    LEGAL_NEW_RESOURCE(mid, client);
    if( (pSrcMap = (ColormapPtr )SecurityLookupIDByType(client,	stuff->srcCmap,
		RT_COLORMAP, DixReadAccess|DixWriteAccess)) )
    {
	result = CopyColormapAndFree(mid, pSrcMap, client->index);
	if (client->noClientException != Success)
            return(client->noClientException);
	else
            return(result);
    }
    else
    {
	client->errorValue = stuff->srcCmap;
	return(BadColor);
    }
}

int
ProcInstallColormap(register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->id,
					    RT_COLORMAP, DixReadAccess);
    if (pcmp)
    {
        (*(pcmp->pScreen->InstallColormap)) (pcmp);
        return (client->noClientException);        
    }
    else
    {
        client->errorValue = stuff->id;
        return (BadColor);
    }
}

int
ProcUninstallColormap(register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->id,
					RT_COLORMAP, DixReadAccess);
    if (pcmp)
    {
	if(pcmp->mid != pcmp->pScreen->defColormap)
            (*(pcmp->pScreen->UninstallColormap)) (pcmp);
        return (client->noClientException);        
    }
    else
    {
        client->errorValue = stuff->id;
        return (BadColor);
    }
}

int
ProcListInstalledColormaps(register ClientPtr client)
{
    xListInstalledColormapsReply *preply; 
    int nummaps;
    WindowPtr pWin;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pWin = (WindowPtr)SecurityLookupWindow(stuff->id, client,
					   DixReadAccess);

    if (!pWin)
        return(BadWindow);

    preply = (xListInstalledColormapsReply *) 
		malloc(sizeof(xListInstalledColormapsReply) +
		     pWin->drawable.pScreen->maxInstalledCmaps *
		     sizeof(Colormap));
    if(!preply)
        return(BadAlloc);

    preply->type = X_Reply;
    preply->sequenceNumber = client->sequence;
    nummaps = (*pWin->drawable.pScreen->ListInstalledColormaps)
        (pWin->drawable.pScreen, (Colormap *)&preply[1]);
    preply->nColormaps = nummaps;
    preply->length = nummaps;
    WriteReplyToClient(client, sizeof (xListInstalledColormapsReply), preply);
    client->pSwapReplyFunc = (ReplySwapPtr) Swap32Write;
    WriteSwappedDataToClient(client, nummaps * sizeof(Colormap), &preply[1]);
    free(preply);
    return(client->noClientException);
}

int
ProcAllocColor (register ClientPtr client)
{
    ColormapPtr pmap;
    int	retval;
    xAllocColorReply acr = {0};
    REQUEST(xAllocColorReq);

    REQUEST_SIZE_MATCH(xAllocColorReq);
    pmap = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					RT_COLORMAP, DixWriteAccess);
    if (pmap)
    {
	acr.type = X_Reply;
	acr.length = 0;
	acr.sequenceNumber = client->sequence;
	acr.red = stuff->red;
	acr.green = stuff->green;
	acr.blue = stuff->blue;
	acr.pixel = 0;
	if( (retval = AllocColor(pmap, &acr.red, &acr.green, &acr.blue,
	                       &acr.pixel, client->index)) )
	{
            if (client->noClientException != Success)
                return(client->noClientException);
	    else
	        return (retval);
	}
#ifdef PANORAMIX
	if (noPanoramiXExtension || !pmap->pScreen->myNum)
#endif
        WriteReplyToClient(client, sizeof(xAllocColorReply), &acr);
	return (client->noClientException);

    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
}

int
ProcAllocNamedColor (register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xAllocNamedColorReq);

    REQUEST_FIXED_SIZE(xAllocNamedColorReq, stuff->nbytes);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					    RT_COLORMAP, DixWriteAccess);
    if (pcmp)
    {
	int		retval;

	xAllocNamedColorReply ancr = {0};

	ancr.type = X_Reply;
	ancr.length = 0;
	ancr.sequenceNumber = client->sequence;

	if(OsLookupColor(pcmp->pScreen->myNum, (char *)&stuff[1], stuff->nbytes,
	                 &ancr.exactRed, &ancr.exactGreen, &ancr.exactBlue))
	{
	    ancr.screenRed = ancr.exactRed;
	    ancr.screenGreen = ancr.exactGreen;
	    ancr.screenBlue = ancr.exactBlue;
	    ancr.pixel = 0;
	    if( (retval = AllocColor(pcmp,
	                 &ancr.screenRed, &ancr.screenGreen, &ancr.screenBlue,
			 &ancr.pixel, client->index)) )
	    {
                if (client->noClientException != Success)
                    return(client->noClientException);
                else
    	            return(retval);
	    }
#ifdef PANORAMIX
	    if (noPanoramiXExtension || !pcmp->pScreen->myNum)
#endif
            WriteReplyToClient(client, sizeof (xAllocNamedColorReply), &ancr);
	    return (client->noClientException);
	}
	else
	    return(BadName);
	
    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
}

int
ProcAllocColorCells (register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xAllocColorCellsReq);

    REQUEST_SIZE_MATCH(xAllocColorCellsReq);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					RT_COLORMAP, DixWriteAccess);
    if (pcmp)
    {
	xAllocColorCellsReply	accr = {0};
	int			npixels, nmasks, retval;
	long			length;
	Pixel			*ppixels, *pmasks;

	npixels = stuff->colors;
	if (!npixels)
	{
	    client->errorValue = npixels;
	    return (BadValue);
	}
	if (stuff->contiguous != xTrue && stuff->contiguous != xFalse)
	{
	    client->errorValue = stuff->contiguous;
	    return (BadValue);
	}
	nmasks = stuff->planes;
	length = ((long)npixels + (long)nmasks) * sizeof(Pixel);
	ppixels = (Pixel *)malloc(length);
	if(!ppixels)
            return(BadAlloc);
	pmasks = ppixels + npixels;

	if( (retval = AllocColorCells(client->index, pcmp, npixels, nmasks, 
				    (Bool)stuff->contiguous, ppixels, pmasks)) )
	{
	    free(ppixels);
            if (client->noClientException != Success)
                return(client->noClientException);
	    else
	        return(retval);
	}
#ifdef PANORAMIX
	if (noPanoramiXExtension || !pcmp->pScreen->myNum)
#endif
	{
	    accr.type = X_Reply;
	    accr.length = length >> 2;
	    accr.sequenceNumber = client->sequence;
	    accr.nPixels = npixels;
	    accr.nMasks = nmasks;
	    WriteReplyToClient(client, sizeof (xAllocColorCellsReply), &accr);
	    client->pSwapReplyFunc = (ReplySwapPtr) Swap32Write;
	    WriteSwappedDataToClient(client, length, ppixels);
	}
	free(ppixels);
        return (client->noClientException);        
    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
}

int
ProcAllocColorPlanes(register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xAllocColorPlanesReq);

    REQUEST_SIZE_MATCH(xAllocColorPlanesReq);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					RT_COLORMAP, DixWriteAccess);
    if (pcmp)
    {
	xAllocColorPlanesReply	acpr = {0};
	int			npixels, retval;
	long			length;
	Pixel			*ppixels;

	npixels = stuff->colors;
	if (!npixels)
	{
	    client->errorValue = npixels;
	    return (BadValue);
	}
	if (stuff->contiguous != xTrue && stuff->contiguous != xFalse)
	{
	    client->errorValue = stuff->contiguous;
	    return (BadValue);
	}
	acpr.type = X_Reply;
	acpr.sequenceNumber = client->sequence;
	acpr.nPixels = npixels;
	length = (long)npixels * sizeof(Pixel);
	ppixels = (Pixel *)malloc(length);
	if(!ppixels)
            return(BadAlloc);
	if( (retval = AllocColorPlanes(client->index, pcmp, npixels,
	    (int)stuff->red, (int)stuff->green, (int)stuff->blue,
	    (Bool)stuff->contiguous, ppixels,
	    &acpr.redMask, &acpr.greenMask, &acpr.blueMask)) )
	{
            free(ppixels);
            if (client->noClientException != Success)
                return(client->noClientException);
	    else
	        return(retval);
	}
	acpr.length = length >> 2;
#ifdef PANORAMIX
	if (noPanoramiXExtension || !pcmp->pScreen->myNum)
#endif
	{
	    WriteReplyToClient(client, sizeof(xAllocColorPlanesReply), &acpr);
	    client->pSwapReplyFunc = (ReplySwapPtr) Swap32Write;
	    WriteSwappedDataToClient(client, length, ppixels);
	}
	free(ppixels);
        return (client->noClientException);        
    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
}

int
ProcFreeColors(register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xFreeColorsReq);

    REQUEST_AT_LEAST_SIZE(xFreeColorsReq);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					RT_COLORMAP, DixWriteAccess);
    if (pcmp)
    {
	int	count;
        int     retval;

	if(pcmp->flags & AllAllocated)
	    return(BadAccess);
	count = ((client->req_len << 2)- sizeof(xFreeColorsReq)) >> 2;
	retval =  FreeColors(pcmp, client->index, count,
	    (Pixel *)&stuff[1], (Pixel)stuff->planeMask);
        if (client->noClientException != Success)
            return(client->noClientException);
        else
	{
	    client->errorValue = clientErrorValue;
            return(retval);
	}

    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
}

int
ProcStoreColors (ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xStoreColorsReq);

    REQUEST_AT_LEAST_SIZE(xStoreColorsReq);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					RT_COLORMAP, DixWriteAccess);
    if (pcmp)
    {
	int	count;
        int     retval;

        count = (client->req_len << 2) - sizeof(xStoreColorsReq);
	if (count % sizeof(xColorItem))
	    return(BadLength);
	count /= sizeof(xColorItem);
	retval = StoreColors(pcmp, count, (xColorItem *)&stuff[1]);
        if (client->noClientException != Success)
            return(client->noClientException);
        else
	{
	    client->errorValue = clientErrorValue;
            return(retval);
	}
    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
}

int
ProcStoreNamedColor (register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xStoreNamedColorReq);

    REQUEST_FIXED_SIZE(xStoreNamedColorReq, stuff->nbytes);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					RT_COLORMAP, DixWriteAccess);
    if (pcmp)
    {
	xColorItem	def = {0};
        int             retval;

	if(OsLookupColor(pcmp->pScreen->myNum, (char *)&stuff[1],
	                 stuff->nbytes, &def.red, &def.green, &def.blue))
	{
	    def.flags = stuff->flags;
	    def.pixel = stuff->pixel;
	    retval = StoreColors(pcmp, 1, &def);
            if (client->noClientException != Success)
                return(client->noClientException);
	    else
		return(retval);
	}
        return (BadName);        
    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
}

int
ProcQueryColors(register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xQueryColorsReq);

    REQUEST_AT_LEAST_SIZE(xQueryColorsReq);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					RT_COLORMAP, DixReadAccess);
    if (pcmp)
    {
	int			count, retval;
	xrgb 			*prgbs;
	xQueryColorsReply	qcr = {0};

	count = ((client->req_len << 2) - sizeof(xQueryColorsReq)) >> 2;
	prgbs = (xrgb *)malloc(count * sizeof(xrgb));
	if(!prgbs && count)
            return(BadAlloc);
	memset(prgbs, 0, count * sizeof(xrgb));
	if( (retval = QueryColors(pcmp, count, (Pixel *)&stuff[1], prgbs)) )
	{
	    if (prgbs) free(prgbs);
	    if (client->noClientException != Success)
                return(client->noClientException);
	    else
	    {
		client->errorValue = clientErrorValue;
	        return (retval);
	    }
	}

	memset(&qcr, 0, sizeof(xQueryColorsReply));
	qcr.type = X_Reply;
	qcr.length = (count * sizeof(xrgb)) >> 2;
	qcr.sequenceNumber = client->sequence;
	qcr.nColors = count;
	WriteReplyToClient(client, sizeof(xQueryColorsReply), &qcr);
	if (count)
	{
	    client->pSwapReplyFunc = (ReplySwapPtr) SQColorsExtend;
	    WriteSwappedDataToClient(client, count * sizeof(xrgb), prgbs);
	}
	if (prgbs) free(prgbs);
	return(client->noClientException);
	
    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
} 

int
ProcLookupColor(register ClientPtr client)
{
    ColormapPtr pcmp;
    REQUEST(xLookupColorReq);

    REQUEST_FIXED_SIZE(xLookupColorReq, stuff->nbytes);
    pcmp = (ColormapPtr)SecurityLookupIDByType(client, stuff->cmap,
					RT_COLORMAP, DixReadAccess);
    if (pcmp)
    {
	xLookupColorReply lcr = {0};

	if(OsLookupColor(pcmp->pScreen->myNum, (char *)&stuff[1], stuff->nbytes,
	                 &lcr.exactRed, &lcr.exactGreen, &lcr.exactBlue))
	{
	    lcr.type = X_Reply;
	    lcr.length = 0;
	    lcr.sequenceNumber = client->sequence;
	    lcr.screenRed = lcr.exactRed;
	    lcr.screenGreen = lcr.exactGreen;
	    lcr.screenBlue = lcr.exactBlue;
	    (*pcmp->pScreen->ResolveColor)(&lcr.screenRed,
	                                   &lcr.screenGreen,
					   &lcr.screenBlue,
					   pcmp->pVisual);
	    WriteReplyToClient(client, sizeof(xLookupColorReply), &lcr);
	    return(client->noClientException);
	}
        return (BadName);        
    }
    else
    {
        client->errorValue = stuff->cmap;
        return (BadColor);
    }
}

int
ProcCreateCursor (register ClientPtr client)
{
    CursorPtr	pCursor;

    register PixmapPtr 	src;
    register PixmapPtr 	msk;
    unsigned char *	srcbits;
    unsigned char *	mskbits;
    unsigned short	width, height;
    long		n;
    CursorMetricRec cm = {0};


    REQUEST(xCreateCursorReq);

    REQUEST_SIZE_MATCH(xCreateCursorReq);
    LEGAL_NEW_RESOURCE(stuff->cid, client);

    src = (PixmapPtr)SecurityLookupIDByType(client, stuff->source,
					      RT_PIXMAP, DixReadAccess);
    msk = (PixmapPtr)SecurityLookupIDByType(client, stuff->mask,
					      RT_PIXMAP, DixReadAccess);
    if (   src == (PixmapPtr)NULL)
    {
	client->errorValue = stuff->source;
	return (BadPixmap);
    }
    if ( msk == (PixmapPtr)NULL)
    {
	if (stuff->mask != None)
	{
	    client->errorValue = stuff->mask;
	    return (BadPixmap);
	}
    }
    else if (  src->drawable.width != msk->drawable.width
	    || src->drawable.height != msk->drawable.height
	    || src->drawable.depth != 1
	    || msk->drawable.depth != 1)
	return (BadMatch);

    width = src->drawable.width;
    height = src->drawable.height;

    if ( stuff->x > width 
      || stuff->y > height )
	return (BadMatch);

    n = BitmapBytePad(width)*height;
    srcbits = (unsigned char *)malloc(n);
    if (!srcbits)
	return (BadAlloc);
    mskbits = (unsigned char *)malloc(n);
    if (!mskbits)
    {
	free(srcbits);
	return (BadAlloc);
    }

    /* zeroing the (pad) bits helps some ddx cursor handling */
    bzero((char *)srcbits, n);
    (* src->drawable.pScreen->GetImage)( (DrawablePtr)src, 0, 0, width, height,
					 XYPixmap, 1, (void *)srcbits);
    if ( msk == (PixmapPtr)NULL)
    {
	register unsigned char *bits = mskbits;
	while (--n >= 0)
	    *bits++ = ~0;
    }
    else
    {
	/* zeroing the (pad) bits helps some ddx cursor handling */
	bzero((char *)mskbits, n);
	(* msk->drawable.pScreen->GetImage)( (DrawablePtr)msk, 0, 0, width,
					height, XYPixmap, 1, (void *)mskbits);
    }
    cm.width = width;
    cm.height = height;
    cm.xhot = stuff->x;
    cm.yhot = stuff->y;
    pCursor = AllocCursor( srcbits, mskbits, &cm,
	    stuff->foreRed, stuff->foreGreen, stuff->foreBlue,
	    stuff->backRed, stuff->backGreen, stuff->backBlue);

    if (pCursor && AddResource(stuff->cid, RT_CURSOR, (void *)pCursor))
	    return (client->noClientException);
    return BadAlloc;
}

int
ProcCreateGlyphCursor (register ClientPtr client)
{
    CursorPtr pCursor;
    int res;

    REQUEST(xCreateGlyphCursorReq);

    REQUEST_SIZE_MATCH(xCreateGlyphCursorReq);
    LEGAL_NEW_RESOURCE(stuff->cid, client);

    res = AllocGlyphCursor(stuff->source, stuff->sourceChar,
			   stuff->mask, stuff->maskChar,
			   stuff->foreRed, stuff->foreGreen, stuff->foreBlue,
			   stuff->backRed, stuff->backGreen, stuff->backBlue,
			   &pCursor, client);
    if (res != Success)
	return res;
    if (AddResource(stuff->cid, RT_CURSOR, (void *)pCursor))
	return client->noClientException;
    return BadAlloc;
}


int
ProcFreeCursor (register ClientPtr client)
{
    CursorPtr pCursor;
    REQUEST(xResourceReq);

    REQUEST_SIZE_MATCH(xResourceReq);
    pCursor = (CursorPtr)SecurityLookupIDByType(client, stuff->id,
					RT_CURSOR, DixDestroyAccess);
    if (pCursor) 
    {
	FreeResource(stuff->id, RT_NONE);
	return (client->noClientException);
    }
    else 
    {
	client->errorValue = stuff->id;
	return (BadCursor);
    }
}

int
ProcQueryBestSize (register ClientPtr client)
{
    xQueryBestSizeReply	reply = {0};
    register DrawablePtr pDraw;
    ScreenPtr pScreen;
    REQUEST(xQueryBestSizeReq);

    REQUEST_SIZE_MATCH(xQueryBestSizeReq);
    if ((stuff->class != CursorShape) && 
	(stuff->class != TileShape) && 
	(stuff->class != StippleShape))
    {
	client->errorValue = stuff->class;
        return(BadValue);
    }
    SECURITY_VERIFY_GEOMETRABLE (pDraw, stuff->drawable, client,
				 DixReadAccess);
    if (stuff->class != CursorShape && pDraw->type == UNDRAWABLE_WINDOW)
	return (BadMatch);
    pScreen = pDraw->pScreen;
    (* pScreen->QueryBestSize)(stuff->class, &stuff->width,
			       &stuff->height, pScreen);
    memset(&reply, 0, sizeof(xQueryBestSizeReply));
    reply.type = X_Reply;
    reply.length = 0;
    reply.sequenceNumber = client->sequence;
    reply.width = stuff->width;
    reply.height = stuff->height;
    WriteReplyToClient(client, sizeof(xQueryBestSizeReply), &reply);
    return (client->noClientException);
}


#ifndef NXAGENT_SERVER
int
ProcSetScreenSaver (register ClientPtr client)
{
    int blankingOption, exposureOption;
    REQUEST(xSetScreenSaverReq);

    REQUEST_SIZE_MATCH(xSetScreenSaverReq);
    blankingOption = stuff->preferBlank;
    if ((blankingOption != DontPreferBlanking) &&
        (blankingOption != PreferBlanking) &&
        (blankingOption != DefaultBlanking))
    {
	client->errorValue = blankingOption;
        return BadValue;
    }
    exposureOption = stuff->allowExpose;
    if ((exposureOption != DontAllowExposures) &&
        (exposureOption != AllowExposures) &&
        (exposureOption != DefaultExposures))
    {
	client->errorValue = exposureOption;
        return BadValue;
    }
    if (stuff->timeout < -1)
    {
	client->errorValue = stuff->timeout;
        return BadValue;
    }
    if (stuff->interval < -1)
    {
	client->errorValue = stuff->interval;
        return BadValue;
    }

    if (blankingOption == DefaultBlanking)
	ScreenSaverBlanking = defaultScreenSaverBlanking;
    else
	ScreenSaverBlanking = blankingOption; 
    if (exposureOption == DefaultExposures)
	ScreenSaverAllowExposures = defaultScreenSaverAllowExposures;
    else
	ScreenSaverAllowExposures = exposureOption;

    if (stuff->timeout >= 0)
	ScreenSaverTime = stuff->timeout * MILLI_PER_SECOND;
    else 
	ScreenSaverTime = defaultScreenSaverTime;
    if (stuff->interval >= 0)
	ScreenSaverInterval = stuff->interval * MILLI_PER_SECOND;
    else
	ScreenSaverInterval = defaultScreenSaverInterval;

    SetScreenSaverTimer();
    return (client->noClientException);
}
#endif /* NXAGENT_SERVER */

int
ProcGetScreenSaver(register ClientPtr client)
{
    xGetScreenSaverReply rep = {0};

    REQUEST_SIZE_MATCH(xReq);
    rep.type = X_Reply;
    rep.length = 0;
    rep.sequenceNumber = client->sequence;
    rep.timeout = ScreenSaverTime / MILLI_PER_SECOND;
    rep.interval = ScreenSaverInterval / MILLI_PER_SECOND;
    rep.preferBlanking = ScreenSaverBlanking;
    rep.allowExposures = ScreenSaverAllowExposures;
    WriteReplyToClient(client, sizeof(xGetScreenSaverReply), &rep);
    return (client->noClientException);
}

int
ProcChangeHosts(register ClientPtr client)
{
    REQUEST(xChangeHostsReq);
    int result;

    REQUEST_FIXED_SIZE(xChangeHostsReq, stuff->hostLength);

    if(stuff->mode == HostInsert)
	result = AddHost(client, (int)stuff->hostFamily,
			 stuff->hostLength, (void *)&stuff[1]);
    else if (stuff->mode == HostDelete)
	result = RemoveHost(client, (int)stuff->hostFamily, 
			    stuff->hostLength, (void *)&stuff[1]);  
    else
    {
	client->errorValue = stuff->mode;
        return BadValue;
    }
    if (!result)
	result = client->noClientException;
    return (result);
}

int
ProcListHosts(register ClientPtr client)
{
    xListHostsReply reply = {0};
    int	len, nHosts, result;
    void *	pdata;
    /* REQUEST(xListHostsReq); */

    REQUEST_SIZE_MATCH(xListHostsReq);
#ifdef XCSECURITY
    /* untrusted clients can't list hosts */
    if (client->trustLevel != XSecurityClientTrusted)
    {
	SecurityAudit("client %d attempted to list hosts\n", client->index);
	return BadAccess;
    }
#endif
    result = GetHosts(&pdata, &nHosts, &len, &reply.enabled);
    if (result != Success)
	return(result);
    reply.type = X_Reply;
    reply.sequenceNumber = client->sequence;
    reply.nHosts = nHosts;
    reply.length = len >> 2;
    WriteReplyToClient(client, sizeof(xListHostsReply), &reply);
    if (nHosts)
    {
	client->pSwapReplyFunc = (ReplySwapPtr) SLHostsExtend;
	WriteSwappedDataToClient(client, len, pdata);
    }
    free(pdata);
    return (client->noClientException);
}

int
ProcChangeAccessControl(register ClientPtr client)
{
    int result;
    REQUEST(xSetAccessControlReq);

    REQUEST_SIZE_MATCH(xSetAccessControlReq);
    if ((stuff->mode != EnableAccess) && (stuff->mode != DisableAccess))
    {
	client->errorValue = stuff->mode;
        return BadValue;
    }
    result = ChangeAccessControl(client, stuff->mode == EnableAccess);
    if (!result)
	result = client->noClientException;
    return (result);
}

int
ProcKillClient(register ClientPtr client)
{
    REQUEST(xResourceReq);
    ClientPtr	killclient;

    REQUEST_SIZE_MATCH(xResourceReq);
    if (stuff->id == AllTemporary)
    {
	CloseDownRetainedResources();
        return (client->noClientException);
    }

    if ((killclient = LookupClient(stuff->id, client)))
    {
	CloseDownClient(killclient);
	/* if an LBX proxy gets killed, isItTimeToYield will be set */
	if (isItTimeToYield || (client == killclient))
	{
	    /* force yield and return Success, so that Dispatch()
	     * doesn't try to touch client
	     */
	    isItTimeToYield = TRUE;
	    return (Success);
	}
	return (client->noClientException);
    }
    else
    {
	client->errorValue = stuff->id;
	return (BadValue);
    }
}

int
ProcSetFontPath(register ClientPtr client)
{
    unsigned char *ptr;
    unsigned long nbytes, total;
    long nfonts;
    int n, result;
    REQUEST(xSetFontPathReq);
    
    REQUEST_AT_LEAST_SIZE(xSetFontPathReq);
    
    nbytes = (client->req_len << 2) - sizeof(xSetFontPathReq);
    total = nbytes;
    ptr = (unsigned char *)&stuff[1];
    nfonts = stuff->nFonts;
    while (--nfonts >= 0)
    {
	if ((total == 0) || (total < (n = (*ptr + 1))))
	    return(BadLength);
	total -= n;
	ptr += n;
    }
    if (total >= 4)
	return(BadLength);
    result = SetFontPath(client, stuff->nFonts, (unsigned char *)&stuff[1]);
    if (!result)
	result = client->noClientException;
    return (result);
}

int
ProcGetFontPath(register ClientPtr client)
{
    xGetFontPathReply reply = {0};
    int stringLens, numpaths;
    unsigned char *bufferStart;
    /* REQUEST (xReq); */

    REQUEST_SIZE_MATCH(xReq);
    bufferStart = GetFontPath(&numpaths, &stringLens);

    reply.type = X_Reply;
    reply.sequenceNumber = client->sequence;
    reply.length = (stringLens + numpaths + 3) >> 2;
    reply.nPaths = numpaths;

    WriteReplyToClient(client, sizeof(xGetFontPathReply), &reply);
    if (stringLens || numpaths)
	WriteToClient(client, stringLens + numpaths, bufferStart);
    return(client->noClientException);
}

int
ProcChangeCloseDownMode(register ClientPtr client)
{
    REQUEST(xSetCloseDownModeReq);

    REQUEST_SIZE_MATCH(xSetCloseDownModeReq);
    if ((stuff->mode == AllTemporary) ||
	(stuff->mode == RetainPermanent) ||
	(stuff->mode == RetainTemporary))
    {
	client->closeDownMode = stuff->mode;
	return (client->noClientException);
    }
    else   
    {
	client->errorValue = stuff->mode;
	return (BadValue);
    }
}

#ifndef NXAGENT_SERVER
int ProcForceScreenSaver(register ClientPtr client)
{    
    REQUEST(xForceScreenSaverReq);

    REQUEST_SIZE_MATCH(xForceScreenSaverReq);
    
    if ((stuff->mode != ScreenSaverReset) && 
	(stuff->mode != ScreenSaverActive))
    {
	client->errorValue = stuff->mode;
        return BadValue;
    }
    SaveScreens(SCREEN_SAVER_FORCER, (int)stuff->mode);
    return client->noClientException;
}
#endif /* NXAGENT_SERVER */

int ProcNoOperation(register ClientPtr client)
{
    REQUEST_AT_LEAST_SIZE(xReq);
    
    /* noop -- don't do anything */
    return(client->noClientException);
}

void
InitProcVectors(void)
{
    int i;
    for (i = 0; i<256; i++)
    {
	if(!ProcVector[i])
	{
            ProcVector[i] = SwappedProcVector[i] = ProcBadRequest;
	    ReplySwapVector[i] = ReplyNotSwappd;
	}
    }
    for(i = LASTEvent; i < 128; i++)
    {
	EventSwapVector[i] = NotImplemented;
    }
    
}


/**********************
 * CloseDownClient
 *
 *  Client can either mark his resources destroy or retain.  If retained and
 *  then killed again, the client is really destroyed.
 *********************/

#ifndef NXAGENT_SERVER
char dispatchExceptionAtReset = DE_RESET;

void
CloseDownClient(register ClientPtr client)
{
    Bool really_close_down = client->clientGone ||
			     client->closeDownMode == DestroyAll;

    if (!client->clientGone)
    {
	/* ungrab server if grabbing client dies */
	if (grabState != GrabNone && grabClient == client)
	{
	    UngrabServer(client);
	}
	BITCLEAR(grabWaiters, client->index);
	DeleteClientFromAnySelections(client);
	ReleaseActiveGrabs(client);
	DeleteClientFontStuff(client);
	if (!really_close_down)
	{
	    /*  This frees resources that should never be retained
	     *  no matter what the close down mode is.  Actually we
	     *  could do this unconditionally, but it's probably
	     *  better not to traverse all the client's resources
	     *  twice (once here, once a few lines down in
	     *  FreeClientResources) in the common case of
	     *  really_close_down == TRUE.
	     */
	    FreeClientNeverRetainResources(client);
	    client->clientState = ClientStateRetained;
  	    if (ClientStateCallback)
            {
		NewClientInfoRec clientinfo;

		clientinfo.client = client; 
		clientinfo.prefix = (xConnSetupPrefix *)NULL;  
		clientinfo.setup = (xConnSetup *) NULL;
		CallCallbacks((&ClientStateCallback), (void *)&clientinfo);
            } 
	}
	client->clientGone = TRUE;  /* so events aren't sent to client */
	if (ClientIsAsleep(client))
	    ClientSignal (client);
	ProcessWorkQueueZombies();
	CloseDownConnection(client);

	/* If the client made it to the Running stage, nClients has
	 * been incremented on its behalf, so we need to decrement it
	 * now.  If it hasn't gotten to Running, nClients has *not*
	 * been incremented, so *don't* decrement it.
	 */
	if (client->clientState != ClientStateInitial &&
	    client->clientState != ClientStateAuthenticating )
	{
	    --nClients;
	}
    }

    if (really_close_down)
    {
	if (client->clientState == ClientStateRunning && nClients == 0)
	    dispatchException |= dispatchExceptionAtReset;

	client->clientState = ClientStateGone;
	if (ClientStateCallback)
	{
	    NewClientInfoRec clientinfo;

	    clientinfo.client = client; 
	    clientinfo.prefix = (xConnSetupPrefix *)NULL;  
	    clientinfo.setup = (xConnSetup *) NULL;
	    CallCallbacks((&ClientStateCallback), (void *)&clientinfo);
	} 	    
	FreeClientResources(client);
	/* Disable client ID tracking. This must be done after
	 * ClientStateCallback. */
	ReleaseClientIds(client);
	if (client->index < nextFreeClientID)
	    nextFreeClientID = client->index;
	clients[client->index] = NullClient;
	SmartLastClient = NullClient;
	free(client);

	while (!clients[currentMaxClients-1])
	    currentMaxClients--;
    }
}
#endif /* NXAGENT_SERVER */

static void
KillAllClients()
{
    int i;
    for (i=1; i<currentMaxClients; i++)
        if (clients[i]) {
            /* Make sure Retained clients are released. */
            clients[i]->closeDownMode = DestroyAll;
            CloseDownClient(clients[i]);     
        }
}

/*********************
 * CloseDownRetainedResources
 *
 *    Find all clients that are gone and have terminated in RetainTemporary 
 *    and  destroy their resources.
 *********************/

void
CloseDownRetainedResources()
{
    register int i;
    register ClientPtr client;

    for (i=1; i<currentMaxClients; i++)
    {
        client = clients[i];
        if (client && (client->closeDownMode == RetainTemporary)
	    && (client->clientGone))
	    CloseDownClient(client);
    }
}

extern int clientPrivateLen;
extern unsigned *clientPrivateSizes;
extern unsigned totalClientSize;

void InitClient(ClientPtr client, int i, void * ospriv)
{
    client->index = i;
    client->sequence = 0; 
    client->clientAsMask = ((Mask)i) << CLIENTOFFSET;
    client->clientGone = FALSE;
    client->closeDownMode = i ? DestroyAll : RetainPermanent;
    client->numSaved = 0;
    client->saveSet = (SaveSetElt *)NULL;
    client->noClientException = Success;
#ifdef DEBUG
    client->requestLogIndex = 0;
#endif
    client->requestVector = InitialVector;
    client->osPrivate = ospriv;
    client->swapped = FALSE;
    client->big_requests = FALSE;
    client->priority = 0;
    client->clientState = ClientStateInitial;
#ifdef XKB
    if (!noXkbExtension) {
	client->xkbClientFlags = 0;
	client->mapNotifyMask = 0;
	QueryMinMaxKeyCodes(&client->minKC,&client->maxKC);
    }
#endif
    client->replyBytesRemaining = 0;
#ifdef XCSECURITY
    client->trustLevel = XSecurityClientTrusted;
    client->CheckAccess = NULL;
    client->authId = 0;
#endif
    client->fontResFunc = NULL;
    client->smart_priority = 0;
    client->smart_start_tick = SmartScheduleTime;
    client->smart_stop_tick = SmartScheduleTime;
    client->smart_check_tick = SmartScheduleTime;
    client->clientIds = NULL;
}

#ifndef NXAGENT_SERVER
int
InitClientPrivates(ClientPtr client)
{
    register char *ptr;
    DevUnion *ppriv;
    register unsigned *sizes;
    register unsigned size;
    register int i;

    if (totalClientSize == sizeof(ClientRec))
	ppriv = (DevUnion *)NULL;
    else if (client->index)
	ppriv = (DevUnion *)(client + 1);
    else
    {
	ppriv = (DevUnion *)malloc(totalClientSize - sizeof(ClientRec));
	if (!ppriv)
	    return 0;
    }
    client->devPrivates = ppriv;
    sizes = clientPrivateSizes;
    ptr = (char *)(ppriv + clientPrivateLen);
    for (i = clientPrivateLen; --i >= 0; ppriv++, sizes++)
    {
	if ( (size = *sizes) )
	{
	    ppriv->ptr = (void *)ptr;
	    ptr += size;
	}
	else
	    ppriv->ptr = (void *)NULL;
    }

    /* Allow registrants to initialize the serverClient devPrivates */
    if (!client->index && ClientStateCallback)
    {
	NewClientInfoRec clientinfo;

	clientinfo.client = client;
	clientinfo.prefix = (xConnSetupPrefix *)NULL;
	clientinfo.setup = (xConnSetup *) NULL;
	CallCallbacks((&ClientStateCallback), (void *)&clientinfo);
    }
    return 1;
}
#endif /* NXAGENT_SERVER */

/************************
 * int NextAvailableClient(ospriv)
 *
 * OS dependent portion can't assign client id's because of CloseDownModes.
 * Returns NULL if there are no free clients.
 *************************/

ClientPtr NextAvailableClient(void * ospriv)
{
    register int i;
    register ClientPtr client;
    xReq data;

    i = nextFreeClientID;
    if (i == MAXCLIENTS)
	return (ClientPtr)NULL;
    clients[i] = client = (ClientPtr)calloc(1,totalClientSize);
    if (!client)
	return (ClientPtr)NULL;
    InitClient(client, i, ospriv);
    InitClientPrivates(client);
    if (!InitClientResources(client))
    {
	free(client);
	return (ClientPtr)NULL;
    }
    data.reqType = 1;
    data.length = (sz_xReq + sz_xConnClientPrefix) >> 2;
    if (!InsertFakeRequest(client, (char *)&data, sz_xReq))
    {
	FreeClientResources(client);
	free(client);
	return (ClientPtr)NULL;
    }
    if (i == currentMaxClients)
	currentMaxClients++;
    while ((nextFreeClientID < MAXCLIENTS) && clients[nextFreeClientID])
	nextFreeClientID++;

    /* Enable client ID tracking. This must be done before
     * ClientStateCallback. */
    ReserveClientIds(client);

    if (ClientStateCallback)
    {
	NewClientInfoRec clientinfo = {0};

        clientinfo.client = client;
        clientinfo.prefix = (xConnSetupPrefix *)NULL;
        clientinfo.setup = (xConnSetup *) NULL;
	CallCallbacks((&ClientStateCallback), (void *)&clientinfo);
    }
    return(client);
}

int
ProcInitialConnection(register ClientPtr client)
{
    REQUEST(xReq);
    register xConnClientPrefix *prefix;
    int whichbyte = 1;
    char order;

    prefix = (xConnClientPrefix *)((char *)stuff + sz_xReq);
    order = prefix->byteOrder;
    if (order != 'l' && order != 'B' && order != 'r' && order != 'R')
	return (client->noClientException = -1);
    if (((*(char *) &whichbyte) && (order == 'B' || order == 'R')) ||
	(!(*(char *) &whichbyte) && (order == 'l' || order == 'r')))
    {
	client->swapped = TRUE;
	SwapConnClientPrefix(prefix);
    }
    stuff->reqType = 2;
    stuff->length += ((prefix->nbytesAuthProto + (unsigned)3) >> 2) +
		     ((prefix->nbytesAuthString + (unsigned)3) >> 2);
    if (client->swapped)
    {
	swaps(&stuff->length);
    }
    if (order == 'r' || order == 'R')
    {
	client->local = FALSE;
    }
    ResetCurrentRequest(client);
    return (client->noClientException);
}

int
SendConnSetup(register ClientPtr client, char *reason)
{
    register xWindowRoot *root;
    register int i;
    int numScreens;
    char* lConnectionInfo;
    xConnSetupPrefix* lconnSetupPrefix;

    if (reason)
    {
	xConnSetupPrefix csp = {0};;

	csp.success = xFalse;
	csp.lengthReason = strlen(reason);
	csp.length = (csp.lengthReason + (unsigned)3) >> 2;
	csp.majorVersion = X_PROTOCOL;
	csp.minorVersion = X_PROTOCOL_REVISION;
	if (client->swapped)
	    WriteSConnSetupPrefix(client, &csp);
	else
	    WriteToClient(client, sz_xConnSetupPrefix, &csp);
        WriteToClient(client, (int)csp.lengthReason, reason);
	return (client->noClientException = -1);
    }

    numScreens = screenInfo.numScreens;
    lConnectionInfo = ConnectionInfo;
    lconnSetupPrefix = &connSetupPrefix;

    /* We're about to start speaking X protocol back to the client by
     * sending the connection setup info.  This means the authorization
     * step is complete, and we can count the client as an
     * authorized one.
     */
    nClients++;

    client->requestVector = client->swapped ? SwappedProcVector : ProcVector;
    client->sequence = 0;
    ((xConnSetup *)lConnectionInfo)->ridBase = client->clientAsMask;
    ((xConnSetup *)lConnectionInfo)->ridMask = RESOURCE_ID_MASK;
#ifdef MATCH_CLIENT_ENDIAN
    ((xConnSetup *)lConnectionInfo)->imageByteOrder = ClientOrder (client);
    ((xConnSetup *)lConnectionInfo)->bitmapBitOrder = ClientOrder (client);
#endif
    /* fill in the "currentInputMask" */
    root = (xWindowRoot *)(lConnectionInfo + connBlockScreenStart);
#ifdef PANORAMIX
    if (noPanoramiXExtension)
	numScreens = screenInfo.numScreens;
    else 
        numScreens = ((xConnSetup *)ConnectionInfo)->numRoots;
#endif

    for (i=0; i<numScreens; i++) 
    {
	register unsigned int j;
	register xDepth *pDepth;

        root->currentInputMask = screenInfo.screens[i]->root->eventMask |
			         wOtherEventMasks (screenInfo.screens[i]->root);
	pDepth = (xDepth *)(root + 1);
	for (j = 0; j < root->nDepths; j++)
	{
	    pDepth = (xDepth *)(((char *)(pDepth + 1)) +
				pDepth->nVisuals * sizeof(xVisualType));
	}
	root = (xWindowRoot *)pDepth;
    }

    if (client->swapped)
    {
	WriteSConnSetupPrefix(client, lconnSetupPrefix);
	WriteSConnectionInfo(client,
			     (unsigned long)(lconnSetupPrefix->length << 2),
			     lConnectionInfo);
    }
    else
    {
	WriteToClient(client, sizeof(xConnSetupPrefix),
			    (char *) lconnSetupPrefix);
	WriteToClient(client, (int)(lconnSetupPrefix->length << 2),
			    lConnectionInfo);
    }
    client->clientState = ClientStateRunning;
    if (ClientStateCallback)
    {
	NewClientInfoRec clientinfo = {0};

        clientinfo.client = client;
        clientinfo.prefix = lconnSetupPrefix;
        clientinfo.setup = (xConnSetup *)lConnectionInfo;
	CallCallbacks((&ClientStateCallback), (void *)&clientinfo);
    }
    return (client->noClientException);
}

int
ProcEstablishConnection(register ClientPtr client)
{
    char *reason, *auth_proto, *auth_string;
    register xConnClientPrefix *prefix;
    REQUEST(xReq);

    prefix = (xConnClientPrefix *)((char *)stuff + sz_xReq);
    auth_proto = (char *)prefix + sz_xConnClientPrefix;
    auth_string = auth_proto + ((prefix->nbytesAuthProto + 3) & ~3);
    if ((prefix->majorVersion != X_PROTOCOL) ||
	(prefix->minorVersion != X_PROTOCOL_REVISION))
	reason = "Protocol version mismatch";
    else
	reason = ClientAuthorized(client,
				  (unsigned short)prefix->nbytesAuthProto,
				  auth_proto,
				  (unsigned short)prefix->nbytesAuthString,
				  auth_string);
    /*
     * If Kerberos is being used for this client, the clientState
     * will be set to ClientStateAuthenticating at this point.
     * More messages need to be exchanged among the X server, Kerberos
     * server, and client to figure out if everyone is authorized.
     * So we don't want to send the connection setup info yet, since
     * the auth step isn't really done.
     */
    if (client->clientState == ClientStateCheckingSecurity)
	client->clientState = ClientStateCheckedSecurity;
    else if (client->clientState != ClientStateAuthenticating)
	return(SendConnSetup(client, reason));
    return(client->noClientException);
}

void
SendErrorToClient(ClientPtr client, unsigned majorCode, unsigned minorCode, 
                  XID resId, int errorCode)
{
    xError rep = {0};

    rep.type = X_Error;
    rep.errorCode = errorCode;
    rep.majorCode = majorCode;
    rep.minorCode = minorCode;
    rep.resourceID = resId;

    WriteEventsToClient (client, 1, (xEvent *)&rep);
}

void
DeleteWindowFromAnySelections(WindowPtr pWin)
{
    register int i;

    for (i = 0; i< NumCurrentSelections; i++)
        if (CurrentSelections[i].pWin == pWin)
        {
	    if (SelectionCallback)
	    {
	        SelectionInfoRec    info;

		info.selection = &CurrentSelections[i];
		info.kind = SelectionWindowDestroy;
		CallCallbacks(&SelectionCallback, &info);
	    }
            CurrentSelections[i].pWin = (WindowPtr)NULL;
            CurrentSelections[i].window = None;
	    CurrentSelections[i].client = NullClient;
	}
}

static void
DeleteClientFromAnySelections(ClientPtr client)
{
    register int i;

    for (i = 0; i< NumCurrentSelections; i++)
        if (CurrentSelections[i].client == client)
        {
	    if (SelectionCallback)
	    {
	        SelectionInfoRec    info;

		info.selection = &CurrentSelections[i];
		info.kind = SelectionWindowDestroy;
		CallCallbacks(&SelectionCallback, &info);
	    }
            CurrentSelections[i].pWin = (WindowPtr)NULL;
            CurrentSelections[i].window = None;
	    CurrentSelections[i].client = NullClient;
	}
}

void
MarkClientException(ClientPtr client)
{
    client->noClientException = -1;
}

/*
 * This array encodes the answer to the question "what is the log base 2
 * of the number of pixels that fit in a scanline pad unit?"
 * Note that ~0 is an invalid entry (mostly for the benefit of the reader).
 */
static int answer[6][4] = {
    /* pad   pad   pad     pad*/
    /*  8     16    32    64 */

    {   3,     4,    5 ,   6 },	/* 1 bit per pixel */
    {   1,     2,    3 ,   4 },	/* 4 bits per pixel */
    {   0,     1,    2 ,   3 },	/* 8 bits per pixel */
    {   ~0,    0,    1 ,   2 },	/* 16 bits per pixel */
    {   ~0,    ~0,   0 ,   1 },	/* 24 bits per pixel */
    {   ~0,    ~0,   0 ,   1 }	/* 32 bits per pixel */
};

/*
 * This array gives the bytesperPixel value for cases where the number
 * of bits per pixel is a multiple of 8 but not a power of 2.
 */
static int answerBytesPerPixel[ 33 ] = {
    ~0, 0, ~0, ~0,	/* 1 bit per pixel */
    0, ~0, ~0, ~0,	/* 4 bits per pixel */
    0, ~0, ~0, ~0,	/* 8 bits per pixel */
    ~0,~0, ~0, ~0,
    0, ~0, ~0, ~0,	/* 16 bits per pixel */
    ~0,~0, ~0, ~0,
    3, ~0, ~0, ~0,	/* 24 bits per pixel */
    ~0,~0, ~0, ~0,
    0		/* 32 bits per pixel */
};

/*
 * This array gives the answer to the question "what is the first index for
 * the answer array above given the number of bits per pixel?"
 * Note that ~0 is an invalid entry (mostly for the benefit of the reader).
 */
static int indexForBitsPerPixel[ 33 ] = {
    ~0, 0, ~0, ~0,	/* 1 bit per pixel */
    1, ~0, ~0, ~0,	/* 4 bits per pixel */
    2, ~0, ~0, ~0,	/* 8 bits per pixel */
    ~0,~0, ~0, ~0,
    3, ~0, ~0, ~0,	/* 16 bits per pixel */
    ~0,~0, ~0, ~0,
    4, ~0, ~0, ~0,	/* 24 bits per pixel */
    ~0,~0, ~0, ~0,
    5		/* 32 bits per pixel */
};

/*
 * This array gives the answer to the question "what is the second index for
 * the answer array above given the number of bits per scanline pad unit?"
 * Note that ~0 is an invalid entry (mostly for the benefit of the reader).
 */
static int indexForScanlinePad[ 65 ] = {
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
     0, ~0, ~0, ~0,	/* 8 bits per scanline pad unit */
    ~0, ~0, ~0, ~0,
     1, ~0, ~0, ~0,	/* 16 bits per scanline pad unit */
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
     2, ~0, ~0, ~0,	/* 32 bits per scanline pad unit */
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
    ~0, ~0, ~0, ~0,
     3		/* 64 bits per scanline pad unit */
};

#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

static int init_screen(ScreenPtr pScreen, int i)
{
    int scanlinepad, format, depth, bitsPerPixel, j, k;
#ifdef DEBUG
    void	(**jNI) ();
#endif /* DEBUG */

    pScreen->myNum = i;
    pScreen->WindowPrivateLen = 0;
    pScreen->WindowPrivateSizes = (unsigned *)NULL;
    pScreen->totalWindowSize =
        ((sizeof(WindowRec) + sizeof(long) - 1) / sizeof(long)) * sizeof(long);
    pScreen->GCPrivateLen = 0;
    pScreen->GCPrivateSizes = (unsigned *)NULL;
    pScreen->totalGCSize =
        ((sizeof(GC) + sizeof(long) - 1) / sizeof(long)) * sizeof(long);
#ifdef PIXPRIV
    pScreen->PixmapPrivateLen = 0;
    pScreen->PixmapPrivateSizes = (unsigned *)NULL;
    pScreen->totalPixmapSize = BitmapBytePad(sizeof(PixmapRec)*8);
#endif
    pScreen->ClipNotify = 0;	/* for R4 ddx compatibility */
    pScreen->CreateScreenResources = 0;

#ifdef DEBUG
    for (jNI = &pScreen->QueryBestSize;
     jNI < (void (**) ()) &pScreen->SendGraphicsExpose;
     jNI++)
    *jNI = NotImplemented;
#endif /* DEBUG */

    /*
     * This loop gets run once for every Screen that gets added,
     * but thats ok.  If the ddx layer initializes the formats
     * one at a time calling AddScreen() after each, then each
     * iteration will make it a little more accurate.  Worst case
     * we do this loop N * numPixmapFormats where N is # of screens.
     * Anyway, this must be called after InitOutput and before the
     * screen init routine is called.
     */
    for (format=0; format<screenInfo.numPixmapFormats; format++)
    {
    depth = screenInfo.formats[format].depth;
    bitsPerPixel = screenInfo.formats[format].bitsPerPixel;
    scanlinepad = screenInfo.formats[format].scanlinePad;
    j = indexForBitsPerPixel[ bitsPerPixel ];
    k = indexForScanlinePad[ scanlinepad ];
    PixmapWidthPaddingInfo[ depth ].padPixelsLog2 = answer[j][k];
    PixmapWidthPaddingInfo[ depth ].padRoundUp =
        (scanlinepad/bitsPerPixel) - 1;
    j = indexForBitsPerPixel[ 8 ]; /* bits per byte */
    PixmapWidthPaddingInfo[ depth ].padBytesLog2 = answer[j][k];
    PixmapWidthPaddingInfo[ depth ].bitsPerPixel = bitsPerPixel;
    if (answerBytesPerPixel[bitsPerPixel])
    {
        PixmapWidthPaddingInfo[ depth ].notPower2 = 1;
        PixmapWidthPaddingInfo[ depth ].bytesPerPixel =
	answerBytesPerPixel[bitsPerPixel];
    }
    else
    {
        PixmapWidthPaddingInfo[ depth ].notPower2 = 0;
    }
    }
    return 0;
}

void FreeScreen(ScreenPtr);

/*
    grow the array of screenRecs if necessary.
    call the device-supplied initialization procedure
    with its screen number, a pointer to its ScreenRec, argc, and argv.
    return the number of successfully installed screens.
*/

int
AddScreen(Bool (*pfnInit) (ScreenPtr /*pScreen */ ,
                           int /*argc */ ,
                           char **      /*argv */
          ), int argc, char **argv)
{

    int i;
    ScreenPtr pScreen;
    Bool ret;

    i = screenInfo.numScreens;
    if (i == MAXSCREENS)
        return -1;

    pScreen = (ScreenPtr) calloc(1, sizeof(ScreenRec));
    if (!pScreen)
        return -1;

    pScreen->devPrivates = (DevUnion *)calloc(sizeof(DevUnion),
                               screenPrivateCount);
    if (!pScreen->devPrivates && screenPrivateCount)
        return -1;

    ret = init_screen(pScreen, i);
    if (ret != 0) {
        free(pScreen);
        return ret;
    }
    /* This is where screen specific stuff gets initialized.  Load the
       screen structure, call the hardware, whatever.
       This is also where the default colormap should be allocated and
       also pixel values for blackPixel, whitePixel, and the cursor
       Note that InitScreen is NOT allowed to modify argc, argv, or
       any of the strings pointed to by argv.  They may be passed to
       multiple screens.
    */
    pScreen->rgf = ~0L;  /* there are no scratch GCs yet*/
    screenInfo.screens[i] = pScreen;
    screenInfo.numScreens++;
    if (!(*pfnInit)(pScreen, argc, argv))
    {
    FreeScreen(pScreen);
    screenInfo.numScreens--;
    return -1;
    }
    return i;
}

void
FreeScreen(ScreenPtr pScreen)
{
    pScreen->root = NullWindow;
    free(pScreen->WindowPrivateSizes);
    free(pScreen->GCPrivateSizes);
#ifdef PIXPRIV
    free(pScreen->PixmapPrivateSizes);
#endif
    free(pScreen->devPrivates);
    free(pScreen);
}
