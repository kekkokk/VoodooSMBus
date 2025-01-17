// SPDX-License-Identifier: GPL-2.0-only
/*
 * ELANTouchpadDriver.cpp
 * Elan I2C/SMBus Touchpad driver port for macOS X
 *
 * Copyright (c) 2019 Leonard Kleinhans <leo-labs>
 *
 * Based on linux driver:
 ********************************************************************************
 *
 * Elan I2C/SMBus Touchpad driver
 *
 * Copyright (c) 2013 ELAN Microelectronics Corp.
 *
 * Author: 林政維 (Duson Lin) <dusonlin@emc.com.tw>
 * Author: KT Liao <kt.liao@emc.com.tw>
 * Version: 1.6.3
 *
 * Based on cyapa driver:
 * copyright (c) 2011-2012 Cypress Semiconductor, Inc.
 * copyright (c) 2011-2012 Google, Inc.
 *
 * Trademarks are the property of their respective owners.
 */
#include "ELANTouchpadDriver.hpp"


#define super IOService

OSDefineMetaClassAndStructors(ELANTouchpadDriver, VoodooSMBusSlaveDeviceDriver);


bool ELANTouchpadDriver::init(OSDictionary *dict) {
    bool result = super::init(dict);
   
    data = reinterpret_cast<elan_tp_data*>(IOMalloc(sizeof(elan_tp_data)));

    transducers = OSArray::withCapacity(ETP_MAX_FINGERS);

    DigitiserTransducerType type = kDigitiserTransducerFinger;
    for (int i = 0; i < ETP_MAX_FINGERS; i++) {
        VoodooI2CDigitiserTransducer* transducer = VoodooI2CDigitiserTransducer::transducer(type, NULL);
        transducers->setObject(transducer);
    }
    awake = true;
    trackpointScrolling = false;
    return result;
}

void ELANTouchpadDriver::free(void) {
    IOFree(data, sizeof(elan_tp_data));
    super::free();
}

void ELANTouchpadDriver::releaseResources() {
    sendSleepCommand();
    OSSafeReleaseNULL(device_nub);
    
    if (transducers) {
        for (int i = 0; i < transducers->getCount(); i++) {
            OSObject* object = transducers->getObject(i);
            OSSafeReleaseNULL(object);
        }
    }
    
    OSSafeReleaseNULL(transducers);
    unpublishMultitouchInterface();
    OSSafeReleaseNULL(mt_interface);
    unpublishTrackpoint();
    OSSafeReleaseNULL(trackpoint);
}

bool ELANTouchpadDriver::start(IOService* provider) {
    if (!super::start(provider)) {
        return false;
    }
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, VoodooI2CIOPMPowerStates, kVoodooI2CIOPMNumberPowerStates);
    
    device_nub->setSlaveDeviceFlags(I2C_CLIENT_HOST_NOTIFY);
    publishMultitouchInterface();
    publishTrackpoint();
    setDeviceParameters();
    
    int error = tryInitialize();
    if(error) {
        IOLogError("Could not initialize ELAN device.");
        return false;
    }
    
    registerService();
    return true;
}


void ELANTouchpadDriver::stop(IOService* provider) {
    releaseResources();
    PMstop();
    super::stop(provider);
}

ELANTouchpadDriver* ELANTouchpadDriver::probe(IOService* provider, SInt32* score) {
    IOLog("Touchpad probe\n");
    if (!super::probe(provider, score)) {
        return NULL;
    }
    
    device_nub = OSDynamicCast(VoodooSMBusDeviceNub, provider);
    if (!device_nub) {
        IOLog("%s Could not get VoodooSMBus device nub instance\n", getName());
        return NULL;
    }
    return this;
}

IOReturn ELANTouchpadDriver::setPowerState(unsigned long whichState, IOService* whatDevice) {
    if (whatDevice != this)
        return kIOPMAckImplied;
    
    if (whichState == kIOPMPowerOff) {
        if (awake) {
            awake = false;
            sendSleepCommand();
        }
    } else {
        if (!awake) {
            IOLogDebug("ELANTouchpadDriver waking up");
            int error = tryInitialize();
            if(error) {
                IOLogError("Could not initialize ELAN device.");
            }
            awake = true;
        }
    }
    return kIOPMAckImplied;
}

bool ELANTouchpadDriver::publishMultitouchInterface() {
    mt_interface = OSTypeAlloc(VoodooI2CMultitouchInterface);
    if (!mt_interface) {
        IOLogError("No memory to allocate VoodooI2CMultitouchInterface instance\n");
        goto multitouch_exit;
    }
    if (!mt_interface->init(NULL)) {
        IOLogError("Failed to init multitouch interface\n");
        goto multitouch_exit;
    }
    if (!mt_interface->attach(this)) {
        IOLogError("Failed to attach multitouch interface\n");
        goto multitouch_exit;
    }
    if (!mt_interface->start(this)) {
        IOLogError("Failed to start multitouch interface\n");
        goto multitouch_exit;
    }
    // Assume we are a touchpad
    mt_interface->setProperty(kIOHIDDisplayIntegratedKey, false);
    mt_interface->registerService();
    return true;
multitouch_exit:
    unpublishMultitouchInterface();
    return false;
}
void ELANTouchpadDriver::unpublishMultitouchInterface() {
    if (mt_interface) {
        mt_interface->stop(this);
    }
}

bool ELANTouchpadDriver::publishTrackpoint() {
    trackpoint = OSTypeAlloc(TrackpointDevice);
    if (!trackpoint) {
        IOLogError("No memory to allocate TrackpointDevice instance\n");
        goto trackpoint_exit;
    }
    if (!trackpoint->init(NULL)) {
        IOLogError("Failed to init TrackpointDevice\n");
        goto trackpoint_exit;
    }
    if (!trackpoint->attach(this)) {
        IOLogError("Failed to attach TrackpointDevice\n");
        goto trackpoint_exit;
    }
    if (!trackpoint->start(this)) {
        IOLogError("Failed to start TrackpointDevice \n");
        goto trackpoint_exit;
    }
  
    trackpoint->registerService();
    return true;
trackpoint_exit:
    unpublishTrackpoint();
    return false;
}
void ELANTouchpadDriver::unpublishTrackpoint() {
    if (trackpoint) {
        trackpoint->stop(this);
    }
}



int ELANTouchpadDriver::tryInitialize() {
    IOSleep(3000);
    int repeat = ETP_RETRY_COUNT;
    int error;
    do {
        error = initialize();
        if (!error)
            return 0;
        
        IOSleep(100);
    } while (--repeat > 0);
    return error;
}

void ELANTouchpadDriver::handleHostNotify() {
    int error;
    u8 report[ETP_MAX_REPORT_LEN];
    error = getReport(report);
    if (error) {
        return;
    }
    
    // Check if input is disabled via ApplePS2Keyboard request
    if (ignoreall) {
        return;
    }
    
    // Ignore input for specified time after keyboard usage
    AbsoluteTime timestamp;
    clock_get_uptime(&timestamp);
    uint64_t timestamp_ns;
    absolutetime_to_nanoseconds(timestamp, &timestamp_ns);
    
    if (timestamp_ns - keytime < maxaftertyping) {
        return;
    }
    
    switch (report[ETP_REPORT_ID_OFFSET]) {
        case ETP_REPORT_ID:
            reportAbsolute(report);
            break;
        case ETP_TP_REPORT_ID:
            reportTrackpoint(report);
            break;
        default:
            IOLogError("invalid report id data (%x)\n",
                    report[ETP_REPORT_ID_OFFSET]);
    }
}



// elan_smbus_initialize
int ELANTouchpadDriver::initialize() {
    UInt8 check[ETP_SMBUS_HELLOPACKET_LEN] = { 0x55, 0x55, 0x55, 0x55, 0x55 };
    UInt8 values[I2C_SMBUS_BLOCK_MAX] = {0};
    int len, error;
    
    /* Get hello packet */
    len = device_nub->readBlockData(ETP_SMBUS_HELLOPACKET_CMD, values);
    
    if (len != ETP_SMBUS_HELLOPACKET_LEN) {
        IOLog("hello packet length fail: %d\n", len);
        error = len < 0 ? len : -EIO;
        return error;
    }
    
    /* compare hello packet */
    if (memcmp(values, check, ETP_SMBUS_HELLOPACKET_LEN)) {
        IOLog("hello packet fail [%*ph]\n",
                ETP_SMBUS_HELLOPACKET_LEN, values);
        return -ENXIO;
    }
    
    /* enable tp */
    error = device_nub->writeByte(ETP_SMBUS_ENABLE_TP);
    if (error) {
        IOLog("failed to enable touchpad: %d\n", error);
        return error;
    }
    
    u8 mode = ETP_ENABLE_ABS;
    error = setMode(mode);
    if (error) {
        IOLogDebug("failed to switch to absolute mode: %d\n", error);
        return error;
    }
    
    return 0;
}

int ELANTouchpadDriver::setMode(u8 mode) {
    u8 cmd[4] = { 0x00, 0x07, 0x00, mode };
    
    return device_nub->writeBlockData(ETP_SMBUS_IAP_CMD, sizeof(cmd), cmd);
}

// TODO lets query stuff
bool ELANTouchpadDriver::setDeviceParameters() {
    
    u8 hw_x_res = 1, hw_y_res = 1;
    unsigned int x_traces = 1, y_traces = 1;
    
    data->max_x = 3052;
    data->max_y = 1888;
    data->width_x = data->max_x / x_traces;
    data->width_y = data->max_y / y_traces;
    
    data->pressure_adjustment = 25;
    
    data->x_res = convertResolution(hw_x_res);
    data->y_res = convertResolution(hw_y_res);
    
    mt_interface->physical_max_x =  data->max_x * 10 / data->x_res;
    mt_interface->physical_max_y = data->max_y * 10 / data->y_res;
    mt_interface->logical_max_x = data->max_x;
    mt_interface->logical_max_y = data->max_y;
    

    return true;
}

// elan_convert_resolution
unsigned int ELANTouchpadDriver::convertResolution(u8 val) {
    /*
     * (value from firmware) * 10 + 790 = dpi
     *
     * We also have to convert dpi to dots/mm (*10/254 to avoid floating
     * point).
     */
    
    return ((int)(char)val * 10 + 790) * 10 / 254;
}


// elan_smbus_get_report
int ELANTouchpadDriver::getReport(u8 *report)
{
    int len;
    
    len = device_nub->readBlockData(ETP_SMBUS_PACKET_QUERY,
                                    &report[ETP_SMBUS_REPORT_OFFSET]);
    if (len < 0) {
        IOLogError("failed to read report data: %d\n", len);
        return len;
    }
    
    if (len != ETP_SMBUS_REPORT_LEN) {
        IOLogError("wrong report length (%d vs %d expected)\n", len,
                   ETP_SMBUS_REPORT_LEN);
        return -EIO;
    }
    
    return 0;
}

void ELANTouchpadDriver::reportTrackpoint(u8 *report) {
    u8 *packet = &report[ETP_REPORT_ID_OFFSET + 1];
    int x = 0, y = 0;
    
    int btn_left = packet[0] & 0x01;
    int btn_right = packet[0] & 0x02;
    int btn_middle = packet[0] & 0x04;

    int button = btn_left | btn_right | btn_middle;
    if ((packet[3] & 0x0F) == 0x06) {
        x = packet[4] - (int)((packet[1] ^ 0x80) << 1);
        y = (int)((packet[2] ^ 0x80) << 1) - packet[5];
    }
    
    if (btn_middle == 4 && x != 0 && y!=0) {
        trackpointScrolling = true;
    }
    if (btn_middle == 0) {
        trackpointScrolling = false;
    }
    if(trackpointScrolling) {
        trackpoint->updateScrollwheel(-y, -x, 0);
    } else {
        trackpoint->updateRelativePointer(x, y, button);
    }
}

// elan_report_contact
void ELANTouchpadDriver::reportContact(VoodooI2CDigitiserTransducer* transducer, bool contact_valid, u8 *finger_data, AbsoluteTime timestamp) {
    unsigned int pos_x, pos_y;
    unsigned int pressure, mk_x, mk_y;
    unsigned int area_x, area_y, major, minor;
    unsigned int scaled_pressure;

    
    if (contact_valid) {
        pos_x = ((finger_data[0] & 0xf0) << 4) |
        finger_data[1];
        pos_y = ((finger_data[0] & 0x0f) << 8) |
        finger_data[2];
        mk_x = (finger_data[3] & 0x0f);
        mk_y = (finger_data[3] >> 4);
        pressure = finger_data[4];
        
        if (pos_x > data->max_x || pos_y > data->max_y) {
            IOLogDebug("[%d] x=%d y=%d over max (%d, %d)",
                    transducer->id, pos_x, pos_y,
                    data->max_x, data->max_y);
            return;
        }
        
        /*
         * To avoid treating large finger as palm, let's reduce the
         * width x and y per trace.
         */
        area_x = mk_x * (data->width_x - ETP_FWIDTH_REDUCE);
        area_y = mk_y * (data->width_y - ETP_FWIDTH_REDUCE);
        
        major = max(area_x, area_y);
        minor = min(area_x, area_y);
        
        scaled_pressure = pressure + data->pressure_adjustment;
        
        if (scaled_pressure > ETP_MAX_PRESSURE)
            scaled_pressure = ETP_MAX_PRESSURE;
        
        transducer->coordinates.x.update(pos_x, timestamp);
        transducer->coordinates.y.update(transducer->logical_max_y - pos_y, timestamp);
        transducer->tip_switch.update(1, timestamp);

    } else {
        transducer->coordinates.x.update(transducer->coordinates.x.last.value, timestamp);
        transducer->coordinates.y.update(transducer->coordinates.y.last.value, timestamp);
        transducer->tip_switch.update(0, timestamp);
    }
}

// elan_report_absolute
void ELANTouchpadDriver::reportAbsolute(u8 *packet) {
    u8 *finger_data = &packet[ETP_FINGER_DATA_OFFSET];
    int i;
    u8 tp_info = packet[ETP_TOUCH_INFO_OFFSET];
    u8 hover_info = packet[ETP_HOVER_INFO_OFFSET];
    bool contact_valid, hover_event;
    
    VoodooI2CMultitouchEvent event;
    event.contact_count = 0;
    event.transducers = transducers;

    AbsoluteTime timestamp;
    clock_get_uptime(&timestamp);
    
    hover_event = hover_info & 0x40;
    for (i = 0; i < ETP_MAX_FINGERS; i++) {
        contact_valid = tp_info & (1U << (3 + i));
        
        VoodooI2CDigitiserTransducer* transducer = OSDynamicCast(VoodooI2CDigitiserTransducer,
                                                                 transducers->getObject(i));

        transducer->id = i;
        transducer->secondary_id = i;
        transducer->logical_max_x = mt_interface->logical_max_x;
        transducer->logical_max_y = mt_interface->logical_max_y;
        transducer->physical_button.update(tp_info & BIT(0), timestamp);
        transducer->type = kDigitiserTransducerFinger;
        transducer->is_valid = contact_valid;
        
        reportContact(transducer, contact_valid, finger_data, timestamp);
        
        if (contact_valid) {
            finger_data += ETP_FINGER_DATA_LEN;
            event.contact_count++;
        }
    }
   
    // send the event into the multitouch interface
    mt_interface->handleInterruptReport(event, timestamp);
}

void ELANTouchpadDriver::sendSleepCommand() {
    device_nub->writeByte(ETP_SMBUS_SLEEP_CMD);
}

IOReturn ELANTouchpadDriver::message(UInt32 type, IOService* provider, void* argument) {
    switch (type) {
        case kKeyboardGetTouchStatus: {
            bool* pResult = (bool*)argument;
            *pResult = !ignoreall;
            break;
        }
        case kKeyboardSetTouchStatus: {
            bool enable = *((bool*)argument);

            // ignoreall is true when trackpad has been disabled
            if (enable == ignoreall) {
                // save state, and update LED
                ignoreall = !enable;
            }
            break;
        }
        case kKeyboardKeyPressTime: {
            //  Remember last time key was pressed
            keytime = *((uint64_t*)argument);
            break;
        }
    }
    
    return kIOReturnSuccess;
}
