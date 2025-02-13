#include <string.h>

#include "rfm69.h"
#include "random.h"
#include "hsem_table.h"
#include "shared_memory.h"

/* defines */
#define CH_NUM          449
#define rfm_cs_low()    LL_GPIO_ResetOutputPin(RFM_CS_GPIO_Port, RFM_CS_Pin)
#define rfm_cs_high()   LL_GPIO_SetOutputPin(RFM_CS_GPIO_Port, RFM_CS_Pin)


/* types */
typedef enum rfm69_state {
    IDLE = 0,
    CONFIG,
    TX,
    RX
} rfm69_state_t;

typedef enum rfm69_mode {
    SLEEP = 0,
    STANDBY,
    FS,
    TRANSMIT,
    RECEIVE,
    LISTEN
} rfm69_mode_t;

typedef struct rfm69_msg {
    uint8_t length;
    uint8_t node;
    uint8_t *payload;
} rfm69_msg_t;

/* variables */
uint8_t test_transmit_flag = 0;
static m7_to_m4_rfm_request_t * rfm_shared_buffer = (m7_to_m4_rfm_request_t *)(0x38000000);
static rfm69_state_t state = IDLE;
/* [i // 61.03515625 for i in range(863000000, 870000001) if i % 61.03515625 == 0] */
static uint32_t freqs[CH_NUM] = { 14139392, 14139648, 14139904, 14140160, 14140416, 14140672, 14140928, 
    14141184, 14141440, 14141696, 14141952, 14142208, 14142464, 14142720, 14142976, 14143232, 14143488,
    14143744, 14144000, 14144256, 14144512, 14144768, 14145024, 14145280, 14145536, 14145792, 14146048,
    14146304, 14146560, 14146816, 14147072, 14147328, 14147584, 14147840, 14148096, 14148352, 14148608,
    14148864, 14149120, 14149376, 14149632, 14149888, 14150144, 14150400, 14150656, 14150912, 14151168,
    14151424, 14151680, 14151936, 14152192, 14152448, 14152704, 14152960, 14153216, 14153472, 14153728,
    14153984, 14154240, 14154496, 14154752, 14155008, 14155264, 14155520, 14155776, 14156032, 14156288,
    14156544, 14156800, 14157056, 14157312, 14157568, 14157824, 14158080, 14158336, 14158592, 14158848,
    14159104, 14159360, 14159616, 14159872, 14160128, 14160384, 14160640, 14160896, 14161152, 14161408,
    14161664, 14161920, 14162176, 14162432, 14162688, 14162944, 14163200, 14163456, 14163712, 14163968,
    14164224, 14164480, 14164736, 14164992, 14165248, 14165504, 14165760, 14166016, 14166272, 14166528,
    14166784, 14167040, 14167296, 14167552, 14167808, 14168064, 14168320, 14168576, 14168832, 14169088,
    14169344, 14169600, 14169856, 14170112, 14170368, 14170624, 14170880, 14171136, 14171392, 14171648,
    14171904, 14172160, 14172416, 14172672, 14172928, 14173184, 14173440, 14173696, 14173952, 14174208,
    14174464, 14174720, 14174976, 14175232, 14175488, 14175744, 14176000, 14176256, 14176512, 14176768,
    14177024, 14177280, 14177536, 14177792, 14178048, 14178304, 14178560, 14178816, 14179072, 14179328,
    14179584, 14179840, 14180096, 14180352, 14180608, 14180864, 14181120, 14181376, 14181632, 14181888,
    14182144, 14182400, 14182656, 14182912, 14183168, 14183424, 14183680, 14183936, 14184192, 14184448,
    14184704, 14184960, 14185216, 14185472, 14185728, 14185984, 14186240, 14186496, 14186752, 14187008,
    14187264, 14187520, 14187776, 14188032, 14188288, 14188544, 14188800, 14189056, 14189312, 14189568,
    14189824, 14190080, 14190336, 14190592, 14190848, 14191104, 14191360, 14191616, 14191872, 14192128,
    14192384, 14192640, 14192896, 14193152, 14193408, 14193664, 14193920, 14194176, 14194432, 14194688,
    14194944, 14195200, 14195456, 14195712, 14195968, 14196224, 14196480, 14196736, 14196992, 14197248,
    14197504, 14197760, 14198016, 14198272, 14198528, 14198784, 14199040, 14199296, 14199552, 14199808,
    14200064, 14200320, 14200576, 14200832, 14201088, 14201344, 14201600, 14201856, 14202112, 14202368,
    14202624, 14202880, 14203136, 14203392, 14203648, 14203904, 14204160, 14204416, 14204672, 14204928,
    14205184, 14205440, 14205696, 14205952, 14206208, 14206464, 14206720, 14206976, 14207232, 14207488,
    14207744, 14208000, 14208256, 14208512, 14208768, 14209024, 14209280, 14209536, 14209792, 14210048,
    14210304, 14210560, 14210816, 14211072, 14211328, 14211584, 14211840, 14212096, 14212352, 14212608,
    14212864, 14213120, 14213376, 14213632, 14213888, 14214144, 14214400, 14214656, 14214912, 14215168,
    14215424, 14215680, 14215936, 14216192, 14216448, 14216704, 14216960, 14217216, 14217472, 14217728,
    14217984, 14218240, 14218496, 14218752, 14219008, 14219264, 14219520, 14219776, 14220032, 14220288,
    14220544, 14220800, 14221056, 14221312, 14221568, 14221824, 14222080, 14222336, 14222592, 14222848,
    14223104, 14223360, 14223616, 14223872, 14224128, 14224384, 14224640, 14224896, 14225152, 14225408,
    14225664, 14225920, 14226176, 14226432, 14226688, 14226944, 14227200, 14227456, 14227712, 14227968,
    14228224, 14228480, 14228736, 14228992, 14229248, 14229504, 14229760, 14230016, 14230272, 14230528,
    14230784, 14231040, 14231296, 14231552, 14231808, 14232064, 14232320, 14232576, 14232832, 14233088,
    14233344, 14233600, 14233856, 14234112, 14234368, 14234624, 14234880, 14235136, 14235392, 14235648,
    14235904, 14236160, 14236416, 14236672, 14236928, 14237184, 14237440, 14237696, 14237952, 14238208,
    14238464, 14238720, 14238976, 14239232, 14239488, 14239744, 14240000, 14240256, 14240512, 14240768,
    14241024, 14241280, 14241536, 14241792, 14242048, 14242304, 14242560, 14242816, 14243072, 14243328,
    14243584, 14243840, 14244096, 14244352, 14244608, 14244864, 14245120, 14245376, 14245632, 14245888,
    14246144, 14246400, 14246656, 14246912, 14247168, 14247424, 14247680, 14247936, 14248192, 14248448,
    14248704, 14248960, 14249216, 14249472, 14249728, 14249984, 14250240, 14250496, 14250752, 14251008,
    14251264, 14251520, 14251776, 14252032, 14252288, 14252544, 14252800, 14253056, 14253312, 14253568,
    14253824, 14254080
};

/* basic SPI RFM functions */
static void rfm_write(uint8_t addr, uint8_t *ptr, uint8_t len);
static void rfm_read(uint8_t addr, uint8_t *ptr, uint8_t len);
/* config functions */
static void rfm_set_pa(uint8_t pa, uint8_t out);
static void rfm_set_carrier(uint32_t calculated_carrier);
static void rfm_set_mode(rfm69_mode_t mode);
static void rfm_set_payload_length(uint8_t value);
static void rfm_set_broadcast_addr(uint8_t addr);
static void rfm_set_node_addr(uint8_t addr);
static uint8_t rfm_config_sync(uint8_t enable, uint8_t length, uint8_t err_tol, uint8_t *data_ptr);
static void rfm_set_config_fifo(uint8_t fifo_mode, uint8_t fifo_threshold);
static void rfm_set_packet_config1(uint8_t pm_fixed_payload_length, uint8_t dc_free, uint8_t crc_on, uint8_t crc_auto_clear_off, uint8_t addr_filtering);
static uint8_t rfm_set_dio_mapping(uint8_t dio, uint8_t val);
static void rfm_set_preamble_length(uint16_t len);
static void rfm_set_bit_rate(uint8_t msb, uint8_t lsb);
/* data tx/rx */
static void rfm_transmit_data(uint8_t *data_ptr, uint8_t len);
static void rfm_receive_data(uint8_t *data_ptr, uint8_t len);


uint8_t RFM69_Init(uint8_t network_id, uint8_t node_id) {
    uint8_t version = RFM69_RegVersion;
    uint32_t seed;
    uint8_t sync_val[] = {'h', 'e', 'l', 'l'};

    rfm_cs_low();
    if (HAL_SPI_Transmit(RFM_SPI, &version, 1, 100) != HAL_OK) {
        rfm_cs_high();
        return 1;
    }
    if (HAL_SPI_Receive(RFM_SPI, &version, 1, 100) != HAL_OK) {
        rfm_cs_high();
        return 1;
    }
    rfm_cs_high();
    if (version != 0x24)  /* from RFM69 datasheet */
        return 1;

    while (HAL_HSEM_IsSemTaken(HSEM_RNG) && !LL_RNG_IsActiveFlag_DRDY(RNG)) {}
    HAL_HSEM_FastTake(HSEM_RNG);
    seed = LL_RNG_ReadRandData32(RNG);
    HAL_HSEM_Release(HSEM_RNG, 0);

    if (Random_Init(seed))
        return 1;

    rfm_set_node_addr(node_id);
    rfm_set_broadcast_addr(255);
    rfm_set_packet_config1(0, 1, 1, 0, 0);
    rfm_set_carrier(14221312);  /* 868 MHz */
    rfm_set_payload_length(5);
    rfm_set_config_fifo(0, 4);
    if(rfm_config_sync(1, 4, 0, sync_val))
        return 1;
    rfm_set_bit_rate(0x0D, 0x05);
    rfm_set_preamble_length(10);
    rfm_set_pa(3, 10);

    rfm_set_mode(STANDBY);

    return 0;
}

void RFM69_Routine(void) {
    while (1) {
        switch (state) {
        case IDLE:
            break;
        case CONFIG:
            break;
        case TX:
            (void)rfm_set_dio_mapping(0, 0);
            // rfm_transmit_data((uint8_t *)"hello", 5);
            (void)rfm_set_mode(TRANSMIT);
            /* wait for PacketSent */
            while(!LL_GPIO_IsInputPinSet(RFM_DIO0_GPIO_Port, RFM_DIO0_Pin)) {}
            (void)rfm_set_mode(STANDBY);

            break;
        case RX:
            break;
        default:
            break;
        }

        if (test_transmit_flag) {
            test_transmit_flag = 0;

#if 1
            rfm_set_dio_mapping(0, 0);
            rfm_transmit_data((uint8_t *)"hello", 5);
            rfm_set_mode(TRANSMIT);
            while(!LL_GPIO_IsInputPinSet(RFM_DIO0_GPIO_Port, RFM_DIO0_Pin)) {}
            rfm_set_mode(STANDBY);

            BSP_LED_Toggle(LED_YELLOW);
#endif
        }

        if (__HAL_HSEM_GET_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M7_TO_M4_RFM))) {
            /* process request from the M7 */
            __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_M7_TO_M4_RFM));
            if (rfm_shared_buffer->request_type == 0)
                rfm_read(rfm_shared_buffer->arg, &(rfm_shared_buffer->payload[0]), 1);
            /* notify M7 */
            HAL_HSEM_FastTake(HSEM_M4_TO_M7);
            HAL_HSEM_Release(HSEM_M4_TO_M7,0);
        }
    }
}


static void rfm_set_pa(uint8_t pa, uint8_t out) {
    uint8_t temp = (pa << 5) | (out & 31);

    rfm_write(RFM69_RegPaLevel, &temp, 1);
}

/*
 *  @brief  changes carrier frequency. default freq is 915 MHz
 *  @param  calculated_carrier = freq / 61.03515625
 */
static void rfm_set_carrier(uint32_t calculated_carrier) {
    uint8_t data[3] = {0, 0, 0};

    /* freq = freqs[Random_FromTS(HAL_GetTick()) % CH_NUM]; */
    data[0] = (uint8_t)((calculated_carrier >> 16) & 0xFF);
    data[1] = (uint8_t)((calculated_carrier >> 8) & 0xFF);
    data[2] = (uint8_t)(calculated_carrier & 0xFF);

    rfm_write(RFM69_RegFrfMsb, data, 3);
}

/*
 *  @brief  changes mode of operation - sleep, standby, fs, tx, rx
 */
static void rfm_set_mode(rfm69_mode_t mode) {
    uint8_t data = 0;

    if (mode != LISTEN)
        data = ((uint8_t) mode) << 2;
    else
        data = 1 << 6;

    rfm_write(RFM69_RegOpMode, &data, 1);
}

/*
 *  @brief  sets payload length (for which mode ??)
 */
static void rfm_set_payload_length(uint8_t value) {
    rfm_write(RFM69_RegPayloadLength, &value, 1);
}

/*
 *  @brief  sets broadcast address
 */
static void rfm_set_broadcast_addr(uint8_t addr) {
    rfm_write(RFM69_RegBroadcastAdrs, &addr, 1);
}

/*
 *  @brief  sets current device address
 */
static void rfm_set_node_addr(uint8_t addr) {
    rfm_write(RFM69_RegNodeAdrs, &addr, 1);
}

/*
 *  @brief  configs Sync word
 ToDo - check if it works fine
 */
static uint8_t rfm_config_sync(uint8_t enable, uint8_t length, uint8_t err_tol, uint8_t *data_ptr) {
    uint8_t data[9] = {0};

    /* actual size in RFM is length + 1 because 1 sync byte is always enabled */
    if (length < 1 || length > 8)
        return 1;

    data[0] = ((enable & 1) << 7) | ((length - 1) << 3) | (err_tol & 7);
    memcpy(data + 1, data_ptr, length);

    rfm_write(RFM69_RegSyncConfig, data, length + 1);

    return 0;
}

/*
 *  @brief  Configs DIO functions; ClockOut remains OFF
 *  @param  dio DIO num 0-5
 *  @param  val dio state value 0-3
 *  @retval 0 - OK
 *  @retval 1 - bad dio value
 */
static uint8_t rfm_set_dio_mapping(uint8_t dio, uint8_t val) {
    static uint8_t dio_map1_state = 0, dio_map2_state = 7;  /* default values of DIO registers */
    uint8_t data = 0;
    uint8_t addr = 0;

    if (dio <= 3)
        addr = RFM69_RegDioMapping1;
    else if (dio <= 5)
        addr = RFM69_RegDioMapping2;
    else
        return 1;

    switch (dio) {
    case 0:
        dio_map1_state |= ((val & 3) << 6);
        data = dio_map1_state;
        break;
    case 1:
        dio_map1_state |= ((val & 3) << 4);
        data = dio_map1_state;
        break;
    case 2:
        dio_map1_state |= ((val & 3) << 2);
        data = dio_map1_state;
        break;
    case 3:
        dio_map1_state |= (val & 3);
        data = dio_map1_state;
        break;
    case 4:
        dio_map2_state |= ((val & 3) << 6);
        data = dio_map2_state;
        break;
    case 5:
        dio_map2_state |= ((val & 3) << 4);
        data = dio_map2_state;
        break;
    default:
        break;
    }

    rfm_write(addr, &data, 1);

    return 0;
}

/*
 *  @brief  set preable length (a length of sequence of 1 an 0 in bytes)
 *  @param  len length of the preamble
 */
static void rfm_set_preamble_length(uint16_t len) {
    uint8_t data[2] = {(uint8_t)((len >> 8) & 0xFF), (uint8_t)(len & 0xFF)};

    rfm_write(RFM69_RegPreambleMsb, data, 2);
}

/*
 *  @brief  set preable length (a length of sequence of 1 an 0 in bytes)
 *  @param  len length value
 */
static void rfm_set_bit_rate(uint8_t msb, uint8_t lsb) {
    uint8_t data[2] = {msb, lsb};

    rfm_write(RFM69_RegBitrateMsb, data, 2);
}


/*
 *  @brief  configs packet mode
 *  @param  pm_fixed_payload_length
 *          0 - Fixed length; 1 - Variable length
 *  @param  dc_free
 *  @param  crc_on
 *          0 - disables CRC; 1 - enables CRC
 *  @param  crc_auto_clear_off
 * 
 *  @param  addr_filtering Defines address based filtering in Rx
 *          00 → None (Off)
 *          01 → Address field must match NodeAddress
 *          10 → Address field must match NodeAddress or BroadcastAddress
 */
static void rfm_set_packet_config1(uint8_t pm_fixed_payload_length, uint8_t dc_free, uint8_t crc_on, uint8_t crc_auto_clear_off, uint8_t addr_filtering) {
    uint8_t data = 0;

    data = (pm_fixed_payload_length & 1) << 7;
    data |= (dc_free & 3) << 5;
    data |= (crc_on & 1) << 4;
    data |= (crc_auto_clear_off & 1) << 3;
    data |= (addr_filtering & 3) << 1;

    rfm_write(RFM69_RegPacketConfig1, &data, 1);
}

/*
 *  @brief  configs fifo workflow
 *  @param  fifo_mode - Defines the condition to start packet transmission 
 *          0 - the number of bytes in the FIFO exceeds FifoThreshold
 *          1 - FifoNotEmpty
 */
static void rfm_set_config_fifo(uint8_t fifo_mode, uint8_t fifo_threshold) {
    uint8_t data = ((fifo_mode & 1) << 7) | (fifo_threshold & 127);

    rfm_write(RFM69_RegFifoThresh, &data, 1);
}

/*
 * @brief   writes data to RFM FIFO
 */
static void rfm_transmit_data(uint8_t *data_ptr, uint8_t len) {
    rfm_write(RFM69_RegFifo, data_ptr, len);
}

/*
 * @brief   read data from RFM FIFO
 */
static void rfm_receive_data(uint8_t *data_ptr, uint8_t len) {
    rfm_read(RFM69_RegFifo, data_ptr, len);
}

static void rfm_write(uint8_t addr, uint8_t *ptr, uint8_t len) {
    rfm_cs_low();
    uint8_t temp = addr | 128;
    // delay_ms_poll(10);
    // /* send addr with write bit */
    // while (!LL_SPI_IsActiveFlag_TXE(RFM_SPI)) {}
    // LL_SPI_TransmitData8(RFM_SPI, addr | 128);
    // while (LL_SPI_IsActiveFlag_BSY(RFM_SPI)) {}

    // while (len--) {
    //     while (!LL_SPI_IsActiveFlag_TXE(RFM_SPI)) {}
    //     LL_SPI_TransmitData8(RFM_SPI, *(ptr++));
    // }

    // while (!LL_SPI_IsActiveFlag_TXE(RFM_SPI)) {}
    // while (LL_SPI_IsActiveFlag_BSY(RFM_SPI)) {}

    HAL_SPI_Transmit(RFM_SPI, &temp, 1, 100);
    HAL_SPI_Transmit(RFM_SPI, ptr, len, 100);

    rfm_cs_high();
}

static void rfm_read(uint8_t addr, uint8_t *ptr, uint8_t len) {
    rfm_cs_low();
    // delay_ms_poll(10);
    // while (!LL_SPI_IsActiveFlag_TXE(RFM_SPI)) {}
    // LL_SPI_TransmitData8(RFM_SPI, addr);
    // while (LL_SPI_IsActiveFlag_BSY(RFM_SPI)) {}

    // /* dummy byte reading */
    // (void)RFM_SPI->DR;

    // while (len--) {
    //     /* dummy data to generate clock */
    //     while (!LL_SPI_IsActiveFlag_TXE(RFM_SPI)) {}
    //     LL_SPI_TransmitData8(RFM_SPI, 0xFF);
    //     while (LL_SPI_IsActiveFlag_BSY(RFM_SPI)) {}

    //     while (!LL_SPI_IsActiveFlag_RXNE(RFM_SPI)) {}
    //     *(ptr++) = LL_SPI_ReceiveData8(RFM_SPI);
    // }

    HAL_SPI_Transmit(RFM_SPI, &addr, 1, 100);
    HAL_SPI_Receive(RFM_SPI, ptr, len, 100);

    rfm_cs_high();
}
