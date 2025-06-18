#include "PluggableUSBAudio.h"
#include "hardware/dma.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#define NUM_CHANNELS 2
#define SAMPLE_FREQ 44100 //Audacity throws a fit if I use anything else
#define BUFFER_DEPTH 1024 //16-bit word depth, not byte. This does not control the sample depth
#define OVERSAMPLING_RATIO 4 //how many ADC samples per audio sample. This is max achievable on Pi Pico

volatile bool buffer1Ready = false;
volatile bool buffer2Ready = false;
volatile int16_t buffer1[BUFFER_DEPTH*OVERSAMPLING_RATIO] __attribute__((aligned(BUFFER_DEPTH*2*OVERSAMPLING_RATIO)));
volatile int16_t buffer2[BUFFER_DEPTH*OVERSAMPLING_RATIO] __attribute__((aligned(BUFFER_DEPTH*2*OVERSAMPLING_RATIO)));
int16_t outputBuffer[BUFFER_DEPTH];
uint8_t *outputBufferPtr = (uint8_t *)outputBuffer;
int16_t high;
int16_t low;
uint highLocation;
uint lowLocation;

USBAudio audio(true, SAMPLE_FREQ, NUM_CHANNELS, SAMPLE_FREQ, NUM_CHANNELS);

dma_channel_config c1;
dma_channel_config c2;

uint dma_channel_1;
uint dma_channel_2;

void dma_handler_irq0(){
  buffer1Ready = true;
  dma_hw->ints0 = 1u << dma_channel_1; //reset interrupt status
}

void dma_handler_irq1(){
  buffer2Ready = true;
  dma_hw->ints1 = 1u << dma_channel_2; //reset interrupt status
}

void setup(){
  while(!audio.configured()){}
  audio.write_wait_ready();

  _gpio_init(23);
  adc_init();
  gpio_set_dir(23,true);
  gpio_put(23,true);
  adc_gpio_init(26); //left channel
  adc_gpio_init(27); //right channel
  //adc_gpio_init(28); //channel 3?
  adc_select_input(0); //start with left channel
  adc_set_round_robin(0b1<<0 | 0b1<<1);
  adc_set_clkdiv((48000000/(SAMPLE_FREQ*2*OVERSAMPLING_RATIO))-1); //sampling frequency calculation
  adc_fifo_setup(true,true,1,false,false);

  dma_channel_1 = dma_claim_unused_channel(true);
  dma_channel_2 = dma_claim_unused_channel(true);

  c1 = dma_channel_get_default_config(dma_channel_1);
  channel_config_set_read_increment(&c1, false);
  channel_config_set_write_increment(&c1, true);
  channel_config_set_ring(&c1,true,log2(BUFFER_DEPTH*2*OVERSAMPLING_RATIO));
  channel_config_set_dreq(&c1, DREQ_ADC);
  channel_config_set_transfer_data_size(&c1, DMA_SIZE_16);
  channel_config_set_enable(&c1, true);
  channel_config_set_chain_to(&c1,dma_channel_2);

  c2 = dma_channel_get_default_config(dma_channel_2);
  channel_config_set_read_increment(&c2, false);
  channel_config_set_write_increment(&c2, true);
  channel_config_set_ring(&c2,true,log2(BUFFER_DEPTH*2*OVERSAMPLING_RATIO));
  channel_config_set_dreq(&c2, DREQ_ADC);
  channel_config_set_transfer_data_size(&c2, DMA_SIZE_16);
  channel_config_set_enable(&c2, true);
  channel_config_set_chain_to(&c2,dma_channel_1);

  dma_channel_configure(dma_channel_1,&c1,&buffer1,&adc_hw->fifo,BUFFER_DEPTH*OVERSAMPLING_RATIO,false);
  dma_channel_configure(dma_channel_2,&c2,&buffer2,&adc_hw->fifo,BUFFER_DEPTH*OVERSAMPLING_RATIO,false);

  dma_channel_set_irq0_enabled(dma_channel_1,true);
  dma_channel_set_irq1_enabled(dma_channel_2,true);
  irq_set_exclusive_handler(DMA_IRQ_0,dma_handler_irq0);
  irq_set_exclusive_handler(DMA_IRQ_1,dma_handler_irq1);

  irq_set_enabled(DMA_IRQ_0,true);
  irq_set_enabled(DMA_IRQ_1,true);
  dma_channel_start(dma_channel_1);
  adc_run(true);
}

void loop(){
  if(buffer1Ready){
    for(uint i=0;i<BUFFER_DEPTH;i+=2){
      outputBuffer[i] = 0;
      outputBuffer[i+1] = 0;

      //the following code takes the four sample values for each channel, throws out the highest and lowest, averages the remaining two, and converts to signed 16-bit PCM
      high = -1;
      low = 4096;

      for(int j=i*OVERSAMPLING_RATIO;j<(i+2)*OVERSAMPLING_RATIO;j+=2){ //find high and low values
        if(buffer1[j]>high){
          high = buffer1[j];
          highLocation = j;
        }
        if(buffer1[j]<low){
          low = buffer1[j];
          lowLocation = j;
        }
      }
      
      if(highLocation==lowLocation) outputBuffer[i] = buffer1[lowLocation]*2;
      else{
        for(uint k=i*OVERSAMPLING_RATIO;k<(i+2)*OVERSAMPLING_RATIO;k+=2){ 
        if(k==highLocation || k==lowLocation) continue; //throw out highest and lowest values
        else outputBuffer[i] += buffer1[k]; //add median values
        }
      }
      
      outputBuffer[i] = (outputBuffer[i]<<(5-(int)log2(OVERSAMPLING_RATIO)))^0x8000; //convert to signed PCM data format

      high = -1;
      low = 4096;

      for(int j=i*OVERSAMPLING_RATIO+1;j<(i+2)*OVERSAMPLING_RATIO+1;j+=2){ //find high and low values
        if(buffer1[j]>high){
          high = buffer1[j];
          highLocation = j;
        }
        if(buffer1[j]<low){
          low = buffer1[j];
          lowLocation = j;
        }
      }

      if(highLocation==lowLocation) outputBuffer[i+1] = buffer1[lowLocation]*2;
      else{
        for(uint k=i*OVERSAMPLING_RATIO+1;k<(i+2)*OVERSAMPLING_RATIO+1;k+=2){
        if(k==highLocation || k==lowLocation) continue; //throw out highest and lowest values
        else outputBuffer[i+1] += buffer1[k]; //add median values
        }
      }
      
      outputBuffer[i+1] = (outputBuffer[i+1]<<(5-(int)log2(OVERSAMPLING_RATIO)))^0x8000; //fast convert to signed PCM data format
    }

    if(audio.write(outputBufferPtr, BUFFER_DEPTH*2)){ //send the audio buffer data over USB
      buffer1Ready = false;
    } 
  }

  if(buffer2Ready){
    for(uint i=0;i<BUFFER_DEPTH;i+=2){
      outputBuffer[i] = 0;
      outputBuffer[i+1] = 0;

      //the following code takes the four sample values for each channel, throws out the highest and lowest, averages the remaining two, and converts to signed 16-bit PCM
      high = -1;
      low = 4096;

      for(int j=i*OVERSAMPLING_RATIO;j<(i+2)*OVERSAMPLING_RATIO;j+=2){ //find high and low values
        if(buffer2[j]>high){
          high = buffer2[j];
          highLocation = j;
        }
        if(buffer2[j]<low){
          low = buffer2[j];
          lowLocation = j;
        }
      }

      if(highLocation==lowLocation) outputBuffer[i] = buffer2[lowLocation]*2;
      else{
        for(uint k=i*OVERSAMPLING_RATIO;k<(i+2)*OVERSAMPLING_RATIO;k+=2){ 
        if(k==highLocation || k==lowLocation) continue; //throw out highest and lowest values
        else outputBuffer[i] += buffer2[k]; //add median values
        }
      }

      outputBuffer[i] = (outputBuffer[i]<<(5-(int)log2(OVERSAMPLING_RATIO)))^0x8000; //convert to signed PCM data format

      high = -1;
      low = 4096;

      for(int j=i*OVERSAMPLING_RATIO+1;j<(i+2)*OVERSAMPLING_RATIO+1;j+=2){ //find high and low values
        if(buffer2[j]>high){
          high = buffer2[j];
          highLocation = j;
        }
        if(buffer2[j]<low){
          low = buffer2[j];
          lowLocation = j;
        }
      }

      if(highLocation==lowLocation) outputBuffer[i+1] = buffer2[lowLocation]*2;
      else{
        for(uint k=i*OVERSAMPLING_RATIO+1;k<(i+2)*OVERSAMPLING_RATIO+1;k+=2){
        if(k==highLocation || k==lowLocation) continue; //throw out highest and lowest values
        else outputBuffer[i+1] += buffer2[k]; //add median values
        }
      }
      outputBuffer[i+1] = (outputBuffer[i+1]<<(5-(int)log2(OVERSAMPLING_RATIO)))^0x8000; //convert to signed PCM data format
    }

    if(audio.write(outputBufferPtr, BUFFER_DEPTH*2)){ //send the audio buffer data over USB
      buffer2Ready = false;
    } 
  }
}

