#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/timer.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

#include "PICO_PIO_Manchester_433MHzOOK_F007.pio.h"

PIO         pio_ManchDelay = pio0;
const uint  sm_ManchDelay  = 1;
const uint  pin_rx    = 16;
spin_lock_t *msgSpinLock;

#define MAX_HEADERBITS  10
#define MAX_MSGBYTS     6
#define OFFSET_MSGFRMT (MAX_MSGBYTS*2)
#define MAX_MSGFRMT     12
#define MAX_MSGCODS   ((MAX_MSGBYTS*2) + MAX_MSGFRMT)

#define MAX_CHANIDX     7
#define MAX_KNWNCHANIDX 4
uint statCntr[MAX_CHANIDX+1] = { 1,1,1,1,1,1,1,1 };
uint statTot[MAX_CHANIDX+1]  = { 0,0,0,0,0,0,0,0 };
uint msgsHiWater = 0;
uint bitsHiWater = 0;

/*-----------------------------------------------------------------*/
#define MAX_BUFMSGS 16
#define MASK_BUFMSGS (MAX_BUFMSGS - 1)
volatile uint msgTail   = 0;
volatile uint msgHead   = 0;
volatile uint msgCntr   = 0;
volatile uint msgOvrRun = 0;
volatile uint msgUndRun = 0;
uint8_t msgByts[MAX_BUFMSGS][MAX_MSGBYTS];
uint8_t msgcods[MAX_MSGCODS];
void putNxtMsg(void) {
    uint32_t flags = spin_lock_blocking( msgSpinLock );
    if (msgCntr >= MAX_BUFMSGS) {
        msgOvrRun++;
        msgCntr = MAX_BUFMSGS;
    } else {
        msgHead = (msgHead + 1) & MASK_BUFMSGS;
        msgCntr++;
        if (msgCntr > msgsHiWater) msgsHiWater = msgCntr;
    }
    spin_unlock(msgSpinLock, flags);
}
void freeLastMsg( void ) {
    uint32_t flags = spin_lock_blocking( msgSpinLock );
    msgTail = (msgTail + 1) & MASK_BUFMSGS;
    if (msgCntr > 0) msgCntr--;
    spin_unlock(msgSpinLock, flags);
}
bool tryMsgBuf( void ) {
    return msgCntr > 0; 
}

/*-----------------------------------------------------------------*/
#define MAX_BUFWRDS 16
#define MASK_BUFWRDS (MAX_BUFWRDS - 1)
uint32_t rxWrdsBuf[MAX_BUFWRDS];
uint rxWrdTail = 0;
uint rxWrdHead = 0;
uint rxWrdCntr = 0;
uint rxOvrRun  = 0;
uint rxUndRun  = 0;
void putNxtWrd(uint32_t nxtWrd) {
    rxWrdsBuf[rxWrdHead] = nxtWrd;
    rxWrdHead = (rxWrdHead + 1) & MASK_BUFWRDS;
    if (rxWrdCntr >= MAX_BUFWRDS) {
        rxOvrRun++;
        rxWrdCntr = MAX_BUFWRDS;
        rxWrdTail = (rxWrdTail + 1) & MASK_BUFWRDS;
    } else {
        rxWrdCntr++;
        if (rxWrdCntr > bitsHiWater) bitsHiWater = rxWrdCntr;
    }
}
uint32_t getNxtWrd( void ) {
    uint wrdOffSet = rxWrdTail;
    rxWrdTail = (rxWrdTail + 1) & MASK_BUFWRDS;
    if (rxWrdCntr > 0) rxWrdCntr--;
    return rxWrdsBuf[wrdOffSet];
}
bool tryWrdBuf( void ) {
    while (!pio_sm_is_rx_fifo_empty(pio_ManchDelay, sm_ManchDelay)) {
        putNxtWrd( pio_sm_get(pio_ManchDelay, sm_ManchDelay) );
    }
    return rxWrdCntr > 0; 
}

/*-----------------------------------------------------------------*/
uint32_t rxBitsBuf;
uint rxBitCnt = 0;
bool tryBitBuf( void ) {
    if (rxBitCnt > 0) {
        return true;
    } else {
        if (tryWrdBuf()) {
            rxBitsBuf = getNxtWrd();
            // printf("%08x\n",rxBitsBuf);
            rxBitCnt = 32;
            return true;
        } else {
            return false;
        }
    }
}
bool getNxtBit_isSet(void) {
    bool isBitSet = (rxBitsBuf & (uint32_t)0x80000000) != 0;
    rxBitsBuf <<= 1;
    if (rxBitCnt > 0) rxBitCnt--;
    else              rxUndRun++;
    return isBitSet;
}

/*-----------------------------------------------------------------*/
const uint8_t lsfrMask[(MAX_MSGBYTS-1)*8] = {
   0x3e, 0x1f, 0x97, 0xd3, 0xf1, 0xe0, 0x70, 0x38,
   0x1c, 0x0e, 0x07, 0x9b, 0xd5, 0xf2, 0x79, 0xa4,  
   0x52, 0x29, 0x8c, 0x46, 0x23, 0x89, 0xdc, 0x6e,
   0x37, 0x83, 0xd9, 0xf4, 0x7a, 0x3d, 0x86, 0x43,  
   0xb9, 0xc4, 0x62, 0x31, 0x80, 0x40, 0x20, 0x10
};
bool parse_bits_callback(struct repeating_timer *t) {
#define WAIT_PREAM 0
#define WAIT_SYNC  1
#define WAIT_MSG   2
    static uint    msgIdx;
    static uint    bytCnt;
    static uint    bitCnt;
    static uint8_t chkSum;
    static uint8_t tmpByt;
    static uint8_t prevChkSum = 100;
    static bool    prevMsg    = false;
    static uint    msgState   = WAIT_PREAM;
    static uint    hdrHits    = 0;
    while (tryBitBuf()) {
        bool nxtBitIsSet = getNxtBit_isSet();
        switch (msgState) {
        case WAIT_PREAM:
            if (nxtBitIsSet) {
                if (hdrHits < MAX_HEADERBITS) hdrHits++;
            } else {
                if (hdrHits >= MAX_HEADERBITS) msgState = WAIT_SYNC;
                hdrHits = 0;
            }
            break;
        case WAIT_SYNC:
            if (nxtBitIsSet) {
                bytCnt = 0;
                bitCnt = 0;
                chkSum = 100;
                msgIdx = msgHead;
                msgState = WAIT_MSG;
            } else {
                msgState = WAIT_PREAM;
            }
            break;
        case WAIT_MSG:
            if (bitCnt == 0) tmpByt = 0;
            tmpByt <<= 1;
            if (nxtBitIsSet) {
                tmpByt |= 1;
                if (bytCnt < (MAX_MSGBYTS-1)) {
                    chkSum ^= lsfrMask[bytCnt*8+bitCnt];
                }
            }
            bitCnt++;
            if (bitCnt >= 8) {
                msgByts[msgIdx][bytCnt] = tmpByt;
                bitCnt = 0;
                bytCnt++;
                if (bytCnt >= MAX_MSGBYTS) {
                    if (msgByts[msgIdx][MAX_MSGBYTS-1] == chkSum) {
                        int chanIdx = ((msgByts[msgIdx][2] >> 4) & 7);
                        statTot[chanIdx]++;
                        if (prevMsg) {
                            if  (prevChkSum == chkSum) {
                                prevMsg = false;
                                putNxtMsg();
                            }
                        } else {
                            prevMsg = true;
                        }
                        prevChkSum = chkSum;
                    }
                    msgState = WAIT_PREAM;
                }
            }
            // printf("byt %d bit %d\n", bytCnt, bitCnt);
            break;
        default:
            break;
        }           
    }
    return true;
}

/*-----------------------------------------------------------------*/
inline void buff2AsciiHex( uint8_t * buffPtr, uint8_t bytVal  ) {
    uint8_t nibHi = bytVal >> 4;
    uint8_t nibLo = bytVal & 0x0F;
    *buffPtr++ = nibHi > 9 ? nibHi + 'A' - 10 : nibHi + '0';
    *buffPtr   = nibLo > 9 ? nibLo + 'A' - 10 : nibLo + '0';
}
uint decode_msg( void ) {
    uint msgIdx  = msgTail;
    uint chanIdx = ((msgByts[msgIdx][2] >> 4) & 7);
    bool battLow =  (msgByts[msgIdx][2] & 0x80) != 0;
    uint tmpRaw  = ((msgByts[msgIdx][2] & 0xF) << 8) | msgByts[msgIdx][3];
    uint chanID  = chanIdx+1;
    uint tmpX2   = tmpRaw * 2u * 5u / 9u;
    int  tmpDegC = tmpX2 & 1 ? (tmpX2+1)/2-400: (tmpX2)/2-400;
    bool isNeg   = tmpDegC < 0;
    uint absDegC = isNeg ? -tmpDegC : tmpDegC;
    for (uint i = 0; i < MAX_MSGBYTS; i++) {
        buff2AsciiHex( &msgcods[i*2], msgByts[msgIdx][i] );
    }
    msgcods[OFFSET_MSGFRMT+0] = chanIdx > MAX_KNWNCHANIDX ? '?' : ' ';
    msgcods[OFFSET_MSGFRMT+1] = ' ';
    msgcods[OFFSET_MSGFRMT+2] = chanID +'0';
    msgcods[OFFSET_MSGFRMT+3] = battLow ? '!':':';
    msgcods[OFFSET_MSGFRMT+4] = ' ';
    msgcods[OFFSET_MSGFRMT+5] = absDegC >= 1000 ?  absDegC/1000    + '0': ' ';
    msgcods[OFFSET_MSGFRMT+6] = absDegC >= 100  ? (absDegC/100)%10 + '0': ' ';
    if (isNeg) {
        if (msgcods[OFFSET_MSGFRMT+6] == ' ') {
            msgcods[OFFSET_MSGFRMT+6] = '-';
        } else {
            if (msgcods[OFFSET_MSGFRMT+5] == ' ') {
                msgcods[OFFSET_MSGFRMT+5] = '-';
            } else {
                msgcods[OFFSET_MSGFRMT+4] = '-';
            }
        }
    }
    msgcods[OFFSET_MSGFRMT+7] = (absDegC/10)%10 + '0';
    msgcods[OFFSET_MSGFRMT+8] = '.';
    msgcods[OFFSET_MSGFRMT+9] = absDegC%10 + '0';
    msgcods[OFFSET_MSGFRMT+10] = 13;
    msgcods[OFFSET_MSGFRMT+11] = 10;
    return chanIdx;
}

/*-----------------------------------------------------------------*/
int main()
{
    stdio_init_all();

    puts("Hello, world!");

    pio_gpio_init(pio_ManchDelay, pin_rx);

    uint offsetManchDelay = pio_add_program(pio_ManchDelay, &manchWithDelay_program);
    pio_sm_config cfgManchDelay = manchWithDelay_program_get_default_config(offsetManchDelay);    

    sm_config_set_in_pins(&cfgManchDelay, pin_rx);
    sm_config_set_jmp_pin(&cfgManchDelay, pin_rx);
    sm_config_set_in_shift(&cfgManchDelay, false, true, 32);
    sm_config_set_fifo_join(&cfgManchDelay, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&cfgManchDelay, 2543.f);
    pio_sm_init(pio_ManchDelay, sm_ManchDelay, offsetManchDelay, &cfgManchDelay);
    pio_sm_clear_fifos(pio_ManchDelay, sm_ManchDelay);
    pio_sm_exec(pio_ManchDelay, sm_ManchDelay, pio_encode_set(pio_x, 1));
    pio_sm_exec(pio_ManchDelay, sm_ManchDelay, pio_encode_set(pio_y, 0));
    pio_sm_set_enabled(pio_ManchDelay, sm_ManchDelay, true);
        
    msgSpinLock = spin_lock_instance(next_striped_spin_lock_num());

    struct repeating_timer timer;
    add_repeating_timer_ms((32*8)/2, parse_bits_callback, NULL, &timer);

    while (true) {
        if (tryMsgBuf()) {
            uint chanIdx = decode_msg();
            freeLastMsg();
            statCntr[chanIdx]++;
            printf("%.*s  [%d %d %d %d %d] id %d seen tot %d HiBits %d HiMsgs %d\n",
                    sizeof(msgcods)-2,msgcods,
                    statCntr[0],statCntr[1],statCntr[2],statCntr[3],statCntr[4],
                    chanIdx+1,statTot[chanIdx], bitsHiWater, msgsHiWater);
        } else {
            sleep_ms(1000);
        }
    }
    bool cancelled = cancel_repeating_timer(&timer);
    pio_sm_set_enabled(pio_ManchDelay, sm_ManchDelay, false);
}
