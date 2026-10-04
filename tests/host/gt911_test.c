#include <assert.h>
#include <stdio.h>
#include "../../src/drivers/gt911.c"

static int levels[64], reset_count, irq_mode, power_offs;
static uint8_t status, responding_address = GT911_ADDRESS;
static unsigned delay_ms;
size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t len=strlen(src); if (size) { size_t n=len<size-1?len:size-1; memcpy(dst,src,n); dst[n]=0; } return len; }
void vTaskDelay(TickType_t ticks) { delay_ms += ticks; }
esp_err_t gpio_config(const gpio_config_t *c)
{ if (c->pin_bit_mask & (1ULL<<10)) irq_mode=c->mode; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    levels[pin]=(int)level;
    if (pin==4 && !level) reset_count++;
    if (pin==2 && level) power_offs++;
    return ESP_OK;
}
esp_err_t solar_os_bus_i2c_transmit_receive(const char *bus, uint8_t addr,
    const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    assert(!strcmp(bus,"i2c0") && tx_len==2);
    if (addr!=responding_address) return ESP_ERR_NOT_FOUND;
    const unsigned reg=(unsigned)tx[0]*256+tx[1];
    memset(rx,0,rx_len);
    if (reg==GT911_REG_PRODUCT_ID) { assert(rx_len==4); memcpy(rx,"911",3); }
    else if (reg==GT911_REG_STATUS) { assert(rx_len==1); *rx=status; }
    else {
        assert(reg==GT911_REG_POINTS && rx_len==8);
        const uint8_t point[]={2,0x34,1,0x56,2,0,0,0}; memcpy(rx,point,8);
    }
    return ESP_OK;
}
esp_err_t solar_os_bus_i2c_transmit(const char *bus, uint8_t addr,
    const uint8_t *tx, size_t length)
{
    assert(!strcmp(bus,"i2c0") && addr==responding_address && length==3);
    assert(tx[0]==0x81 && tx[1]==0x4e && tx[2]==0); status=0; return ESP_OK;
}
int main(void)
{
    assert(gt911_init_with_reset("i2c0",0x5d,0x14,10,4,2,0)==ESP_OK);
    assert(reset_count==1 && levels[2]==0 && levels[4]==1 && levels[10]==0);
    assert(irq_mode==GPIO_MODE_INPUT && delay_ms==170);
    assert(gt911_init_with_reset("i2c0",0x5d,0x14,10,4,2,0)==ESP_OK && reset_count==1);
    gt911_sample_t sample;
    status=0x81;
    assert(gt911_read(&sample)==ESP_OK && sample.valid && sample.touched && !sample.home);
    assert(sample.x==0x134 && sample.y==0x256 && sample.id==2 && !status);
    assert(gt911_read(&sample)==ESP_OK && !sample.valid);
    status=0x90;
    assert(gt911_read(&sample)==ESP_OK && sample.valid && sample.home && !sample.touched);
    status=0x80;
    assert(gt911_read(&sample)==ESP_OK && sample.valid && !sample.home && !sample.touched);
    gt911_deinit(); assert(power_offs==1);
    responding_address=0x14; reset_count=0;
    assert(gt911_init_with_reset("i2c0",0x5d,0x14,10,4,2,0)==ESP_OK);
    assert(reset_count==2 && levels[10]==1);
    gt911_deinit(); reset_count=0;
    assert(gt911_init("i2c0",0x5d,0x14,10)==ESP_OK && reset_count==0);
    gt911_deinit();
    assert(gt911_init_with_reset("i2c0",0x5d,0x14,10,10,2,0)==ESP_ERR_INVALID_ARG);
    puts("GT911 reset, address and frame tests passed");
}
