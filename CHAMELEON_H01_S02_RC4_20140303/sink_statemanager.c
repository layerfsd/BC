/****************************************************************************
Copyright (C) Cambridge Silicon Radio Ltd. 2004-2013

FILE NAME
    sink_statemanager.h
    
DESCRIPTION
    state machine helper functions used for state changes etc - provide single 
    state change points etc for the sink device application
*/

#include "sink_statemanager.h"
#include "sink_led_manager.h"
#include "sink_buttonmanager.h"
#include "sink_dut.h"
#include "sink_pio.h"
#include "sink_audio.h"
#include "sink_scan.h"
#include "sink_slc.h"
#include "sink_inquiry.h"
#include "sink_devicemanager.h"
#include "sink_powermanager.h"
#include "sink_speech_recognition.h" 
#include "psu.h"
#include "sink_audio_routing.h"

#ifdef ENABLE_FM
#include "sink_fm.h"
#endif

#ifdef ENABLE_DISPLAY
#include "sink_display.h"
#endif

#ifdef ENABLE_AVRCP
#include "sink_avrcp.h"    
#endif

#ifdef ENABLE_SUBWOOFER
#include "sink_swat.h"
#include <bdaddr.h>
#endif

#ifdef TEST_HARNESS
#include "test_sink.h"
#endif

#include <stdlib.h>
#include <boot.h>

#ifdef DEBUG_STATES
#define SM_DEBUG(x) DEBUG(x)

const char * const gHSStateStrings [ SINK_NUM_STATES ] = {
                               "Limbo",
                               "Connectable",
                               "ConnDisc",
                               "Connected",
                               "Out",
                               "Inc",
                               "ActiveCallSCO",
                               "TESTMODE",
                               "TWCWaiting",
                               "TWCOnHold",
                               "TWMulticall",
                               "IncCallOnHold",
                               "ActiveNoSCO",
                               "deviceA2DPStreaming",
                               "deviceLowBattery"} ;
                               
#else
#define SM_DEBUG(x) 
#endif

#define SM_LIMBO_TIMEOUT_SECS 5

/****************************************************************************
VARIABLES
*/

    /*the device state variable - accessed only from below fns*/
static sinkState gTheSinkState ;


/****************************************************************************
FUNCTIONS
*/

static void stateManagerSetState ( sinkState pNewState ) ;
static void stateManagerResetPIOs ( void ) ;

/****************************************************************************
NAME	
	stateManagerSetState

DESCRIPTION
	helper function to Set the current device state
    provides a single state change point and passes the information
    on to the managers requiring state based responses
    
RETURNS
	
    
*/
static void stateManagerSetState ( sinkState pNewState )
{
	SM_DEBUG(("SM:[%s]->[%s][%d]\n",gHSStateStrings[stateManagerGetState()] , gHSStateStrings[pNewState] , pNewState ));
    
    if ( pNewState < SINK_NUM_STATES )
    {

        if (pNewState != gTheSinkState )
        {
                /*inform the LED manager of the current state to be displayed*/
            LEDManagerIndicateState ( pNewState ) ;
            
#ifdef TEST_HARNESS
            vm2host_send_state(pNewState);
#endif
            
#ifdef ENABLE_DISPLAY
            /* this should be called before the state is updated below */
            displayUpdateAppState(pNewState);
#endif                                  
        }
        else
        {
            /*we are already indicating this state no need to re*/
        }
   
        gTheSinkState = pNewState ;
        BootSetPreservedWord(pNewState);
    }
    else
    {
        SM_DEBUG(("SM: ? [%s] [%x]\n",gHSStateStrings[ pNewState] , pNewState)) ;
    }
    
    if(powerManagerIsChargerDisabled())
    {
        /*if we are in chargererror then reset the leds and reset the error*/
        MessageSend(&theSink.task, EventCancelLedIndication, 0);
    }

}


/****************************************************************************
NAME	
	stateManagerGetState

DESCRIPTION
	helper function to get the current device state

RETURNS
	the Devie State information
    
*/
sinkState stateManagerGetState ( void )
{
    return gTheSinkState ;
}


/****************************************************************************
NAME	
	stateManagerEnterConnectableState

DESCRIPTION
	single point of entry for the connectable state - enters discoverable state 
    if configured to do so

RETURNS
	void
    
*/
void stateManagerEnterConnectableState ( bool req_disc )
{
    sinkState lOldState = stateManagerGetState() ;
    
    /* Don't go connectable if we're still pairing */
    if(!req_disc && theSink.inquiry.action == rssi_pairing)
        return;

    if ( stateManagerIsConnected() && req_disc )
    {       /*then we have an SLC active*/
       sinkDisconnectAllSlc();
    }
        /*disable the ring PIOs if enabled*/
    stateManagerResetPIOs();
        /* Make the device connectable */
    sinkEnableConnectable();
    stateManagerSetState ( deviceConnectable ) ;

        /* if remain discoverable at all times feature is enabled then make the device 
           discoverable in the first place */
    if (theSink.features.RemainDiscoverableAtAllTimes)
    {
        /* Make the device discoverable */  
        sinkEnableDiscoverable();    
    }
    
        /*determine if we have got here after a DiscoverableTimeoutEvent*/
    if ( lOldState == deviceConnDiscoverable )
    {
        /*disable the discoverable mode*/
        if (!theSink.features.RemainDiscoverableAtAllTimes)
        {
            sinkDisableDiscoverable();
        }
        MessageCancelAll ( &theSink.task , EventPairingFail ) ;        
    }
    else
    {
        uint16 lNumDevices = ConnectionTrustedDeviceListSize();
        
        /*if we want to auto enter pairing mode*/
        if ( theSink.features.pair_mode_en )
        {    
            stateManagerEnterConnDiscoverableState( req_disc );
        }  
        SM_DEBUG(("SM: Disco %X RSSI %X\n", theSink.features.DiscoIfPDLLessThan, theSink.features.PairIfPDLLessThan));

        
        /* check whether the RSSI pairing if PDL < feature is set and there are less paired devices than the
           level configured, if so start rssi pairing */
        if((theSink.features.PairIfPDLLessThan)&&( lNumDevices < theSink.features.PairIfPDLLessThan ))
        {
            SM_DEBUG(("SM: NumD [%d] <= PairD [%d]\n" , lNumDevices , theSink.features.PairIfPDLLessThan))

            /* send event to enter pairing mode, that event can be used to play a tone if required */
            MessageSend(&theSink.task, EventEnterPairingEmptyPDL, 0);
            MessageSend(&theSink.task, EventRssiPair, 0);
        }           
        /* otherwise if any of the other action on power on features are enabled... */
        else if (theSink.features.DiscoIfPDLLessThan)
        {
    		SM_DEBUG(("SM: Num Devs %d\n",lNumDevices));
            /* Check if we want to go discoverable */
    		if ( lNumDevices < theSink.features.DiscoIfPDLLessThan )
    		{
    			SM_DEBUG(("SM: NumD [%d] <= DiscoD [%d]\n" , lNumDevices , theSink.features.DiscoIfPDLLessThan))
                /* send event to enter pairing mode, that event can be used to play a tone if required */
                MessageSend(&theSink.task, EventEnterPairingEmptyPDL, 0);
    		}
#ifdef ENABLE_SUBWOOFER
            /* If there is only one paired device and it is the subwoofer, start pairing mode */
            else if ( (lNumDevices == 1) && (!BdaddrIsZero(&theSink.rundata->subwoofer.bd_addr)) )
            {
                /* Check the subwoofer exists in the paired device list */
                theSink.rundata->subwoofer.check_pairing = 1;
                ConnectionSmGetAuthDevice(&theSink.task, &theSink.rundata->subwoofer.bd_addr);
            }
#endif
        }
	}
}

/****************************************************************************
NAME	
	stateManagerEnterConnDiscoverableState

DESCRIPTION
	single point of entry for the connectable / discoverable state 
    uses timeout if configured
RETURNS
	void
    
*/
void stateManagerEnterConnDiscoverableState ( bool req_disc )
{
    /* cancel any pending connection attempts when entering pairing mode */
    MessageCancelAll(&theSink.task, EventContinueSlcConnectRequest);

    if(theSink.features.DoNotDiscoDuringLinkLoss && HfpLinkLoss())
    {
        /*if we are in link loss and do not want to go discoverable during link loss then ignore*/                    
    }
    else
    {    
        /* if in connected state disconnect any still attached devices */
        if ( stateManagerIsConnected() && req_disc )
        {
           /* do we have an SLC active? */
           sinkDisconnectAllSlc();
           /* or an a2dp connection active? */
           disconnectAllA2dpAvrcp();
        }  
     
        /* Make the device connectable */
        sinkEnableConnectable();
    
        /* Make the device discoverable */  
        sinkEnableDiscoverable();    
        
        /* If there is a timeout - send a user message*/
		if ( theSink.conf1->timeouts.PairModeTimeoutIfPDL_s != 0 )
		{
			/* if there are no entries in the PDL, then use the second
			   pairing timer */
			uint16 lNumDevices = ConnectionTrustedDeviceListSize();
			if( lNumDevices != 0)
			{	/* paired devices in list, use original timer if it exists */
				if( theSink.conf1->timeouts.PairModeTimeout_s != 0 )
				{
					SM_DEBUG(("SM : Pair [%x]\n" , theSink.conf1->timeouts.PairModeTimeout_s )) ;
					MessageSendLater ( &theSink.task , EventPairingFail , 0 , D_SEC(theSink.conf1->timeouts.PairModeTimeout_s) ) ;
				}
			}
			else
			{	/* no paired devices in list, use secondary timer */
	            SM_DEBUG(("SM : Pair (no PDL) [%x]\n" , theSink.conf1->timeouts.PairModeTimeoutIfPDL_s )) ;
				MessageSendLater ( &theSink.task , EventPairingFail , 0 , D_SEC(theSink.conf1->timeouts.PairModeTimeoutIfPDL_s) ) ;
			}			            			
		}
        else if ( theSink.conf1->timeouts.PairModeTimeout_s != 0 )
        {
            SM_DEBUG(("SM : Pair [%x]\n" , theSink.conf1->timeouts.PairModeTimeout_s )) ;
            
            MessageSendLater ( &theSink.task , EventPairingFail , 0 , D_SEC(theSink.conf1->timeouts.PairModeTimeout_s) ) ;
        }
        else
        {
            SM_DEBUG(("SM : Pair Indefinetely\n")) ;
        }
        /* Disable the ring PIOs if enabled*/
        stateManagerResetPIOs();
       
    	/* The device is now in the connectable/discoverable state */
        stateManagerSetState(deviceConnDiscoverable);
    }
}


/****************************************************************************
NAME	
	stateManagerEnterConnectedState

DESCRIPTION
	single point of entry for the connected state - disables disco / connectable modes
RETURNS
	void
    
*/
void stateManagerEnterConnectedState ( void )
{
    if((stateManagerGetState () != deviceConnected) && (theSink.inquiry.action != rssi_pairing))
    {
            /*make sure we are now neither connectable or discoverable*/
        SM_DEBUG(("SM:Remain in Disco Mode [%c]\n", (theSink.features.RemainDiscoverableAtAllTimes?'T':'F') )) ;
        
        if (!theSink.features.RemainDiscoverableAtAllTimes)
        {
#ifdef ENABLE_SUBWOOFER     
            if(SwatGetSignallingSink(theSink.rundata->subwoofer.dev_id))
            {
               sinkDisableDiscoverable();            
            }        
#else
            sinkDisableDiscoverable();            
#endif        
        }
        
        /* for multipoint connections need to remain connectable after 1 device connected */
        if(!theSink.MultipointEnable)
        {
#ifdef ENABLE_SUBWOOFER     
            if(SwatGetSignallingSink(theSink.rundata->subwoofer.dev_id))
            {
               sinkDisableConnectable();            
            }        
#else
            sinkDisableConnectable();            
#endif        
        }
        else
        {
            /* if both profiles are now connected disable connectable mode */
            if(theSink.no_of_profiles_connected == MAX_MULTIPOINT_CONNECTIONS)
            {
                /* two devices connected */
                sinkDisableConnectable();                    
            }
            else
            {
                /* still able to connect another devices */
                sinkEnableConnectable();                    
                /* remain connectable for a further 'x' seconds to allow a second 
                   AG to be connected if non-zero, otherwise stay connecatable forever */
                if(theSink.conf1->timeouts.ConnectableTimeout_s)
                {
                    MessageSendLater(&theSink.task, EventConnectableTimeout, 0, D_SEC(theSink.conf1->timeouts.ConnectableTimeout_s));
                }
            }
        }
    
        switch ( stateManagerGetState() )
        {    
            case deviceIncomingCallEstablish:
                if (theSink.conf1->timeouts.MissedCallIndicateTime_s != 0 && 
                    theSink.conf1->timeouts.MissedCallIndicateAttemps != 0)
                {
                    theSink.MissedCallIndicated = theSink.conf1->timeouts.MissedCallIndicateAttemps;
                    
                    MessageSend   (&theSink.task , EventMissedCall , 0 ) ; 
                    
            		MessageSend ( &theSink.task , EventSpeechRecognitionStop , 0 ) ;
            
                }
            case deviceActiveCallSCO:            
            case deviceActiveCallNoSCO:       
            case deviceThreeWayCallWaiting:
            case deviceThreeWayCallOnHold:
            case deviceThreeWayMulticall:
            case deviceOutgoingCallEstablish:
                    /*then we have just ended a call*/
                MessageSend ( &theSink.task , EventEndOfCall , 0 ) ;
            break ;
            default:
            break ;
        }
    
	
        MessageCancelAll ( &theSink.task , EventPairingFail ) ;
        
            /*disable the ring PIOs if enabled*/
        stateManagerResetPIOs();
    
        /* when returning to connected state, check for the prescence of any A2DP instances
           if found enter the appropriate state */
        if((theSink.a2dp_link_data->connected[a2dp_primary])||(theSink.a2dp_link_data->connected[a2dp_secondary]))
        {
            SM_DEBUG(("SM:A2dp connected\n")) ;
            /* are there any A2DP instances streaming audio? */
            if((A2dpMediaGetState(theSink.a2dp_link_data->device_id[a2dp_primary], theSink.a2dp_link_data->stream_id[a2dp_primary]) == a2dp_stream_streaming)||
               (A2dpMediaGetState(theSink.a2dp_link_data->device_id[a2dp_secondary], theSink.a2dp_link_data->stream_id[a2dp_secondary]) == a2dp_stream_streaming))
            {
                SM_DEBUG(("SM:A2dp connected - still streaming\n")) ;
                stateManagerSetState( deviceA2DPStreaming );
            }
            else
                stateManagerSetState( deviceConnected );               
        }
        /* no A2DP connections, go to connected state */
        else 
            stateManagerSetState( deviceConnected );
   }     
}
/****************************************************************************
NAME	
	stateManagerEnterIncomingCallEstablishState

DESCRIPTION
	single point of entry for the incoming call establish state
RETURNS
	void
    
*/
void stateManagerEnterIncomingCallEstablishState ( void )
{
   
    stateManagerSetState( deviceIncomingCallEstablish );
        
    	/*if we enter this state directly*/
    MessageCancelAll ( &theSink.task , EventPairingFail ) ;
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.IncomingRingPIO , pio_drive, TRUE) ;
#ifdef ENABLE_SPEECH_RECOGNITION
    /* speech recognition not support on HSP profile AG's */
    if ((speechRecognitionIsEnabled()) && (!HfpPriorityIsHsp(HfpLinkPriorityFromCallState(hfp_call_state_incoming)))) 
        MessageSend ( &theSink.task , EventSpeechRecognitionStart , 0 ) ;    
#endif    
}

/****************************************************************************
NAME	
	stateManagerEnterOutgoingCallEstablishState

DESCRIPTION
	single point of entry for the outgoing call establish state
RETURNS
	void
    
*/
void stateManagerEnterOutgoingCallEstablishState ( void )
{
    stateManagerSetState( deviceOutgoingCallEstablish );
    
    	/*if we enter this state directly*/
    MessageCancelAll ( &theSink.task , EventPairingFail ) ;
	
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.OutgoingRingPIO , pio_drive, TRUE) ;
}
/****************************************************************************
NAME	
	stateManagerEnterActiveCallState

DESCRIPTION
	single point of entry for the active call state
RETURNS
	void
    
*/
void stateManagerEnterActiveCallState ( void )   
{
	if((stateManagerGetState() == deviceOutgoingCallEstablish) ||
	   (stateManagerGetState() == deviceIncomingCallEstablish))
	{
		/* If a call is being answered then send call answered event */
		MessageSend ( &theSink.task , EventCallAnswered , 0 ) ;		
        MessageSend ( &theSink.task , EventSpeechRecognitionStop , 0 ) ;
	}
	
    if (theSink.routed_audio)
    {	
        stateManagerSetState(  deviceActiveCallSCO );
    }
    else
    {
        stateManagerSetState( deviceActiveCallNoSCO );
    }
	
        /*disable the ring PIOs if enabled*/
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.IncomingRingPIO , pio_drive, FALSE) ;
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.OutgoingRingPIO , pio_drive, FALSE) ;
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.CallActivePIO ,   pio_drive, TRUE) ;
	
    /*if we enter this state directly*/
    MessageCancelAll ( &theSink.task , EventPairingFail ) ;

}


/****************************************************************************
NAME	
	stateManagerEnterA2dpStreamingState

DESCRIPTION
    enter A2DP streaming state if not showing any active call states
RETURNS
	void
    
*/
void stateManagerEnterA2dpStreamingState(void)
{
    /* only allow change to A2DP connected state if not currently showing 
       any active call states */
    if(stateManagerGetState() == deviceConnected) 
	{
        stateManagerSetState(  deviceA2DPStreaming );
	}
}


/****************************************************************************
NAME	
	stateManagerPowerOn

DESCRIPTION
	Power on the deviece by latching on the power regs
RETURNS
	void
    
*/
void stateManagerPowerOn( void ) 
{
    SM_DEBUG(("--hello--\nSM : PowerOn\n"));

	/* Check for DUT mode enable */
	if(!checkForDUTModeEntry())
	{
        /* Reset caller ID flag */
        theSink.RepeatCallerIDFlag = TRUE;

        /*cancel the event power on message if there was one*/
    	MessageCancelAll ( &theSink.task , EventLimboTimeout ) ;
        MessageCancelAll(&theSink.task, EventContinueSlcConnectRequest);

    	PioSetPowerPin ( TRUE ) ;
    
    	PioSetPio ( theSink.conf1->PIOIO.pio_outputs.PowerOnPIO , pio_drive, TRUE) ;
        
		stateManagerEnterConnectableState( TRUE );
        
        if(theSink.features.PairIfPDLLessThan || theSink.features.AutoReconnectPowerOn || theSink.panic_reconnect)
        {
            uint16 lNumDevices = ConnectionTrustedDeviceListSize();
        
            /* Check if we want to start RSSI pairing */
            if( lNumDevices < theSink.features.PairIfPDLLessThan )
            {
                SM_DEBUG(("SM: NumD [%d] <= PairD [%d]\n" , lNumDevices , theSink.features.PairIfPDLLessThan))

                /* send event to enter pairing mode, that event can be used to play a tone if required */
                MessageSend(&theSink.task, EventEnterPairingEmptyPDL, 0);
                MessageSend(&theSink.task, EventRssiPair, 0);
            }
            else if ((theSink.features.AutoReconnectPowerOn)||theSink.panic_reconnect)
            {
                sinkEvents_t event = EventEstablishSLC;
#ifdef ENABLE_AVRCP
                if(theSink.features.avrcp_enabled)
                {
                    sinkAvrcpCheckManualConnectReset(NULL);
                }
#endif                
                SM_DEBUG(("SM: Auto Reconnect\n")) ;
                if(theSink.panic_reconnect)
                {
                    switch(theSink.rundata->old_state)
                    {
                        case deviceLimbo:
                        case deviceConnectable:
                        case deviceConnDiscoverable:
                        case deviceTestMode:
                            event = EventInvalid;
                            break;
                        default:
                            event = EventEstablishSLCOnPanic;
                            break;
                    }
                }

                if(event != EventInvalid) 
                    MessageSend ( &theSink.task , event , 0 ) ;
                else
                    theSink.panic_reconnect = FALSE;
            }
        }
        audioHandleRouting(audio_source_none);
	}
}

/****************************************************************************
NAME	
	stateManagerIsConnected

DESCRIPTION
    Helper method to see if we are connected or not   
*/
bool stateManagerIsConnected ( void )
{
    bool lIsConnected = FALSE ;
    
    switch (stateManagerGetState() )
    {
        case deviceLimbo:
        case deviceConnectable:
        case deviceConnDiscoverable:
        case deviceTestMode:
            lIsConnected = FALSE ;    
        break ;
        
        default:
            lIsConnected = TRUE ;
        break ;
    }
    return lIsConnected ;
}

/****************************************************************************
NAME	
	stateManagerEnterLimboState

DESCRIPTION
    method to provide a single point of entry to the limbo /poweringOn state
RETURNS	
    
*/
void stateManagerEnterLimboState ( void )
{
    
    SM_DEBUG(("SM: Enter Limbo State[%d]\n" , theSink.conf1->timeouts.LimboTimeout_s)); 
    
	/*Make sure panic reconnect flag is reset*/
    theSink.panic_reconnect = FALSE;

    /* turn off AMP pio drive */    
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.DeviceAudioActivePIO , pio_drive, FALSE) ;

    /*Cancel inquiry if in progress*/
    inquiryStop();

    /* Disconnect any slc's and cancel any further connection attempts including link loss */
    MessageCancelAll(&theSink.task, EventStreamEstablish);
    MessageCancelAll(&theSink.task, EventContinueSlcConnectRequest);
    MessageCancelAll(&theSink.task, EventLinkLoss);
    sinkDisconnectAllSlc();     
    
    /* disconnect any a2dp signalling channels */
    disconnectAllA2dpAvrcp();

    /* reset the pdl list indexes in preparation for next boot */
    slcReset();
    
    /*in limbo, the internal regs must be latched on (smps and LDO)*/
    PioSetPowerPin ( TRUE ) ;

#ifndef ENABLE_SOUNDBAR
    /*make sure we are neither connectable or discoverable*/
    sinkDisableDiscoverable();    
    sinkDisableConnectable();  
#endif /* ENABLE_SOUNDBAR */ 

    /*set a timeout so that we will turn off eventually anyway*/
    MessageSendLater ( &theSink.task , EventLimboTimeout , 0 , D_SEC(theSink.conf1->timeouts.LimboTimeout_s) ) ;

    stateManagerSetState( deviceLimbo );

    /* reconnect usb if applicable */
    audioHandleRouting(audio_source_none);
}

/****************************************************************************
NAME	
	stateManagerUpdateLimboState

DESCRIPTION
    method to update the limbo state and power off when necessary
RETURNS	
    
*/
void stateManagerUpdateLimboState ( void ) 
{
    /* determine if charger is still connected */
    bool chg_connected = powerManagerIsChargerConnected();
    bool power_off_request = FALSE;
    
    SM_DEBUG(("SM: Limbo Update\n"));
  
    /* determine if device can be turned off, hs can power off with VregEn High unless 
       feature is set specifying it should be low to allow power down, critical battery
       and over temperature alerts can also power down the device */
    if (!chg_connected)
    {
        /* charger not connected, check if Vbat is critical or temperature is outside of operating limits. */
        if(powerManagerIsVbatCritical() || powerManagerIsVthmCritical())
        {
            /* turn off power as outside of safe operating limits */
            power_off_request = TRUE;
        }
        /* now check for wired audio or FM connections */
        else 
        {
            /* if wired audio and FM is not in use */
#if defined(ENABLE_FM) && defined(ENABLE_WIRED)
            if((!wiredAudioConnected())&&!(IsSinkFmRxOn()))
#elif ENABLE_FM
            if(!IsSinkFmRxOn())
#elif  ENABLE_WIRED
            if(!wiredAudioConnected())
#endif
            {
                /* check power of only if vreg is low feature bit */
                if((!theSink.features.PowerOffOnlyIfVRegEnLow)||(theSink.features.PowerOffOnlyIfVRegEnLow && !PsuGetVregEn()))
                {
                    /* safe to power off */                    
                    power_off_request = TRUE;
                }
            }     
        }
    }
           
    /* check if need to power off unit */    
    if(power_off_request)    
    {
      SM_DEBUG(("SM : Power Off\n--goodbye--\n")) ;
      /* Store DSP data */
      configManagerWriteDspData();
      /* Check defrag */
      configManagerDefragCheck();
      /* Used as a power on indication if required */
      PioSetPio(theSink.conf1->PIOIO.pio_outputs.PowerOnPIO , pio_drive, FALSE) ;
      /* Physically power off */
      PioSetPowerPin(FALSE);
    }
    /* device was not able to power down at this point, schedule another check */
    else
    {
        MessageCancelAll ( &theSink.task , EventLimboTimeout ) ;
        MessageSendLater ( &theSink.task , EventLimboTimeout , 0 , D_SEC(theSink.conf1->timeouts.LimboTimeout_s) );            
    }
}

/****************************************************************************
NAME	
	stateManagerEnterTestModeState

DESCRIPTION
    method to provide a single point of entry to the test mode state
RETURNS	
    
*/
void stateManagerEnterTestModeState ( void )
{
    stateManagerSetState( deviceTestMode );
}

/****************************************************************************
NAME	
	stateManagerEnterCallWaitingState

DESCRIPTION
    method to provide a single point of entry to the 3 way call waiting state
RETURNS	
    
*/
void stateManagerEnterThreeWayCallWaitingState ( void ) 
{
    stateManagerSetState( deviceThreeWayCallWaiting );
	
    	/*if we enter this state directly*/
    MessageCancelAll ( &theSink.task , EventPairingFail ) ;
    
    /* if a single AG is connected with multipoint enabled and hs is in twc call waiting, disable
       connectable to prevent a second AG from connecting */
    if((theSink.MultipointEnable)&&(theSink.no_of_profiles_connected < MAX_MULTIPOINT_CONNECTIONS))
    {
        /* make hs no longer connectable */
#ifdef ENABLE_SUBWOOFER     
        if(SwatGetSignallingSink(theSink.rundata->subwoofer.dev_id))
        {
           sinkDisableConnectable();            
        }        
#else
        sinkDisableConnectable();            
#endif        
    }
}


void stateManagerEnterThreeWayCallOnHoldState ( void ) 
{   
    stateManagerSetState( deviceThreeWayCallOnHold );
	
    	/*if we enter this state directly*/
    MessageCancelAll ( &theSink.task , EventPairingFail ) ;
}

void stateManagerEnterThreeWayMulticallState ( void ) 
{
    stateManagerSetState( deviceThreeWayMulticall );
	
    	/*if we enter this state directly*/
    MessageCancelAll ( &theSink.task , EventPairingFail ) ;
}


void stateManagerEnterIncomingCallOnHoldState ( void )
{
    switch ( stateManagerGetState() )
    {    
        case deviceIncomingCallEstablish:
        	MessageSend ( &theSink.task , EventSpeechRecognitionStop , 0 ) ;
        break ;
        default:
        break ;
    }    
     
    stateManagerSetState( deviceIncomingCallOnHold );
	
    	/*if we enter this state directly*/
    MessageCancelAll ( &theSink.task , EventPairingFail ) ;
}

static void stateManagerResetPIOs ( void )
{
    /*disable the ring PIOs if enabled*/
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.IncomingRingPIO , pio_drive, FALSE) ;
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.OutgoingRingPIO , pio_drive, FALSE) ;
    /*diaable the active call PIO if there is one*/
    PioSetPio ( theSink.conf1->PIOIO.pio_outputs.CallActivePIO ,   pio_drive, FALSE) ;
     
}

