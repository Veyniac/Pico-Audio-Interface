# Pico-Audio-Interface

## Description
This project is a cheap, bare-bones 2-channel USB audio interface based off of a Pi Pico board.

## Features
- 2 channels
- 12-bit resolution
- 44.1 kHz sample rate
- 4x oversampling to reduce USB noise
- -14dB to 1.6dB gain control
- Component cost <$10
- Plug and play (at least on my machine)
- Smaller than a credit card

## Disclaimers
This is not an audiophile-friendly device. Depending on what you plug it into, there may be significant USB noise.
I have done everything I can to reduce that noise, and the audio quality is great when tested with my setup, but 
be aware that your mileage may vary. My goal for this project was to make something that worked, not to try to
compete with commercial audio interfaces.

I am also not responsible if this somehow fries your USB port. Always be careful when connecting a USB cable to an
externally powered device.
