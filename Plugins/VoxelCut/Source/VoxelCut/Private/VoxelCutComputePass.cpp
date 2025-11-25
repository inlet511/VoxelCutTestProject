#include "VoxelCutComputePass.h"

IMPLEMENT_GLOBAL_SHADER(FVoxelCutCS, TEXT("/Project/Shaders/VoxelCutCS.usf"), TEXT("MainCS"), SF_Compute);