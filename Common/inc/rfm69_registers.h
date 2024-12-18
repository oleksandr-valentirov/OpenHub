#pragma once

/* https://www.rcscomponents.kiev.ua/datasheets/rfm69hcw-datasheet.pdf */

/* Common Configuration Registers */
#define RFM69_RegFifo          0x00
#define RFM69_RegOpMode        0x01
#define RFM69_RegDataModul     0x02
#define RFM69_RegBitrateMsb    0x03
#define RFM69_RegBitrateLsb    0x04
#define RFM69_RegFdevMsb       0x05
#define RFM69_RegFdevLsb       0x06
#define RFM69_RegFrfMsb        0x07
#define RFM69_RegFrfMid        0x08
#define RFM69_RegFrfLsb        0x09
#define RFM69_RegOsc1          0x0A
#define RFM69_RegAfcCtrl       0x0B
#define RFM69_Reserved0C       0x0C
#define RFM69_RegListen1       0x0D
#define RFM69_RegListen2       0x0E
#define RFM69_RegListen3       0x0F
#define RFM69_RegVersion       0x10

/* Transmitter Registers */
#define RFM69_RegPaLevel
#define RFM69_RegPaRamp
#define RFM69_RegOcp

/* Receiver Registers */
#define RFM69_RegLna            0x18
#define RFM69_RegRxBw           0x19
#define RFM69_RegAfcBw          0x1A
#define RFM69_RegOokPeak        0x1B
#define RFM69_RegOokAvg         0x1C
#define RFM69_RegOokFix         0x1D
#define RFM69_RegAfcFei         0x1E
#define RFM69_RegAfcMsb         0x1F
#define RFM69_RegAfcLsb         0x20
#define RFM69_RegFeiMsb         0x21
#define RFM69_RegFeiLsb         0x22
#define RFM69_RegRssiConfig     0x23
#define RFM69_RegRssiValue      0x24

/* IRQ and Pin Mapping Registers */
#define RFM69_RegDioMapping1    0x25    /* DIO0 - DIO3 */
#define RFM69_RegDioMapping2    0x26    /* DIO4, DIO5, ClkOut */
#define RFM69_RegPreambleMsb    0x2C

/* Packet Engine Registers */
#define RFM69_RegSyncConfig     0x2E
#define RFM69_RegSyncValue1     0x2F
#define RFM69_RegSyncValue2     0x30
#define RFM69_RegSyncValue3     0x31
#define RFM69_RegSyncValue4     0x32
#define RFM69_RegSyncValue5     0x33
#define RFM69_RegSyncValue6     0x34
#define RFM69_RegSyncValue7     0x35
#define RFM69_RegSyncValue8     0x36
#define RFM69_RegPacketConfig1  0x37
#define RFM69_RegPayloadLength  0x38
#define RFM69_RegNodeAdrs       0x39
#define RFM69_RegBroadcastAdrs  0x3A
#define RFM69_RegFifoThresh     0x3C
/* Temperature Sensor Registers */

/* Test Registers */
