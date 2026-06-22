#pragma once

void  noise_init(unsigned int seed);
float noise_fbm2d(float x, float y, int octaves, float lacunarity, float gain);
float noise_fbm3d(float x, float y, float z, int octaves, float lacunarity, float gain);
