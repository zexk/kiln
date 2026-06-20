#pragma once

void  noise_init(unsigned int seed);
float noise_perlin2d(float x, float y);
float noise_fbm2d(float x, float y, int octaves, float lacunarity, float gain);
