/**
* Copyright 2014 Benjamin Worpitz
*
* This file is part of alpaka.
*
* alpaka is free software: you can redistribute it and/or modify
* it under the terms of either the GNU General Public License or
* the GNU Lesser General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* alpaka is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License and the GNU Lesser General Public License
* for more details.
*
* You should have received a copy of the GNU General Public License
* and the GNU Lesser General Public License along with alpaka.
* If not, see <http://www.gnu.org/licenses/>.
*/

#include <alpaka/alpaka.hpp>    // alpaka::createKernelExecutor<...>

#include <chrono>               // std::chrono::high_resolution_clock
#include <cassert>              // assert
#include <iostream>             // std::cout
#include <vector>               // std::vector
#include <typeinfo>             // typeid
#include <utility>              // std::forward

#ifdef ALPAKA_CUDA_ENABLED
    #include <cuda.h>
#endif

//#############################################################################
//! An accelerated test kernel.
//! Uses atomicOp(), syncBlockKernels(), shared memory, getIdx, getSize, global memory to compute a (useless) result.
//! \tparam TAcc The accelerator environment to be executed on.
//! \tparam TuiNumUselessWork The number of useless calculations done in each kernel execution.
//#############################################################################
template<typename TuiNumUselessWork, typename TAcc = boost::mpl::_1>
class ExampleAcceleratedKernel :
    public alpaka::IAcc<TAcc>
{
public:
    //-----------------------------------------------------------------------------
    //! Constructor.
    //-----------------------------------------------------------------------------
    ExampleAcceleratedKernel(std::uint32_t const uiMult = 2) :
        m_uiMult(uiMult)
    {}

    //-----------------------------------------------------------------------------
    //! The kernel.
    //-----------------------------------------------------------------------------
    ALPAKA_FCT_HOST_ACC void operator()(std::uint32_t * const puiBlockRetVals, std::uint32_t const uiMult2) const
    {
        // The number of kernels in this block.
        std::uint32_t const uiNumKernelsInBlock(getSize<alpaka::Block, alpaka::Kernels, alpaka::Linear>());

        // Get the extern allocated shared memory.
        std::uint32_t * const pBlockShared(getBlockSharedExternMem<std::uint32_t>());

        // Get some shared memory (allocate a second buffer directly afterwards to check for some synchronization bugs).
        //std::uint32_t * const pBlockShared1(allocBlockSharedMem<std::uint32_t, TuiNumUselessWork::value>());
        //std::uint32_t * const pBlockShared2(allocBlockSharedMem<std::uint32_t, TuiNumUselessWork::value>());

        // Calculate linearized index of the kernel in the block.
        std::uint32_t const uiIdxBlockKernelsLin(getIdx<alpaka::Block, alpaka::Kernels, alpaka::Linear>());


        // Fill the shared block with the kernel ids [1+X, 2+X, 3+X, ..., #Threads+X].
        std::uint32_t iSum1(uiIdxBlockKernelsLin+1);
        for(std::uint32_t i(0); i<TuiNumUselessWork::value; ++i)
        {
            iSum1 += i;
        }
        pBlockShared[uiIdxBlockKernelsLin] = iSum1;


        // Synchronize all kernels because now we are writing to the memory again but inverse.
        syncBlockKernels();

        // Do something useless.
        std::uint32_t iSum2(uiIdxBlockKernelsLin);
        for(std::uint32_t i(0); i<TuiNumUselessWork::value; ++i)
        {
            iSum2 -= i;
        }
        // Add the inverse so that every cell is filled with [#Kernels, #Kernels, ..., #Kernels].
        pBlockShared[(uiNumKernelsInBlock-1)-uiIdxBlockKernelsLin] += iSum2;


        // Synchronize all kernels again.
        syncBlockKernels();

        // Now add up all the cells atomically and write the result to cell 0 of the shared memory.
        if(uiIdxBlockKernelsLin > 0)
        {
            atomicOp<alpaka::Add>(&pBlockShared[0], pBlockShared[uiIdxBlockKernelsLin]);
        }


        syncBlockKernels();

        // Only master writes result to global memory.
        if(uiIdxBlockKernelsLin==0)
        {
            // Calculate linearized block id.
            std::uint32_t const bId(getIdx<alpaka::Grid, alpaka::Blocks, alpaka::Linear>());

            puiBlockRetVals[bId] = pBlockShared[0] * m_uiMult * uiMult2;
        }
    }

public:
    std::uint32_t /*const*/ m_uiMult;
};

namespace alpaka
{
    //#############################################################################
    //! The trait for getting the size of the block shared extern memory for a kernel.
    //#############################################################################
    template<class TuiNumUselessWork, class TAcc>
    struct BlockSharedExternMemSizeBytes<ExampleAcceleratedKernel<TuiNumUselessWork, TAcc>>
    {
        //-----------------------------------------------------------------------------
        //! \return The size of the shared memory allocated for a block.
        //-----------------------------------------------------------------------------
        template<typename... TArgs>
        static std::size_t getBlockSharedExternMemSizeBytes(alpaka::vec<3u> const & v3uiSizeBlockKernels, TArgs && ...)
        {
            return v3uiSizeBlockKernels.prod() * sizeof(std::uint32_t);
        }
    };
}

//-----------------------------------------------------------------------------
//! Profiles the given kernel.
//-----------------------------------------------------------------------------
template<typename TExec, typename TWorkSize, typename... TArgs>
void profileAcceleratedKernel(TExec const & exec, alpaka::IWorkSize<TWorkSize> const & workSize, TArgs && ... args)
{
    std::cout
        << "profileAcceleratedKernel("
        << " kernelExecutor: " << typeid(TExec).name()
        << ", workSize: " << workSize
        << ")" << std::endl;

    auto const tpStart(std::chrono::high_resolution_clock::now());

    // Execute the accelerated kernel.
    exec(workSize, std::forward<TArgs>(args)...);

    auto const tpEnd(std::chrono::high_resolution_clock::now());

    auto const durElapsed(tpEnd - tpStart);

    std::cout << "Execution time: " << std::chrono::duration_cast<std::chrono::milliseconds>(durElapsed).count() << " ms" << std::endl;
}

//-----------------------------------------------------------------------------
//! Profiles the example kernel and checks the result.
//-----------------------------------------------------------------------------
template<typename TAcc, typename TuiNumUselessWork, typename TWorkSize>
void profileAcceleratedExampleKernel(alpaka::IWorkSize<TWorkSize> const & workSize, std::uint32_t const uiMult2)
{
    using TKernel = ExampleAcceleratedKernel<TuiNumUselessWork>;
    using TAccMemorySpace = typename TAcc::MemorySpace;

    std::cout
        << "AcceleratedExampleKernelProfiler("
        << " accelerator: " << typeid(TAcc).name()
        << ", kernel: " << typeid(TKernel).name()
        << ")" << std::endl;

    std::size_t const uiNumBlocksInGrid(workSize.template getSize<alpaka::Grid, alpaka::Blocks, alpaka::Linear>());
    std::size_t const uiNumKernelsInBlock(workSize.template getSize<alpaka::Block, alpaka::Kernels, alpaka::Linear>());

    // An array for the return values calculated by the blocks.
    std::vector<std::uint32_t> vuiBlockRetVals(uiNumBlocksInGrid, 0);

    // Allocate accelerator buffers and copy.
    std::size_t const uiSizeBytes(uiNumBlocksInGrid * sizeof(std::uint32_t));
    auto * const pBlockRetVals(alpaka::memory::memAlloc<TAccMemorySpace, std::uint32_t>(uiSizeBytes));
    alpaka::memory::memCopy<TAccMemorySpace, alpaka::MemorySpaceHost>(pBlockRetVals, vuiBlockRetVals.data(), uiSizeBytes);

    std::uint32_t const m_uiMult(42);

    auto exec(alpaka::createKernelExecutor<TAcc, TKernel>(m_uiMult));
    profileAcceleratedKernel(exec, workSize, pBlockRetVals, uiMult2);

    // Copy back the result.
    alpaka::memory::memCopy<alpaka::MemorySpaceHost, TAccMemorySpace>(vuiBlockRetVals.data(), pBlockRetVals, uiSizeBytes);
    alpaka::memory::memFree<TAccMemorySpace>(pBlockRetVals);

    // Assert that the results are correct.
    std::uint32_t const uiCorrectResult(static_cast<std::uint32_t>(uiNumKernelsInBlock*uiNumKernelsInBlock) * m_uiMult * uiMult2);

    bool bResultCorrect(true);
    for(std::size_t i(0); i<uiNumBlocksInGrid; ++i)
    {
        if(vuiBlockRetVals[i] != uiCorrectResult)
        {
            std::cout << "vuiBlockRetVals[" << i << "] == " << vuiBlockRetVals[i] << " != " << uiCorrectResult << std::endl;
            bResultCorrect = false;
        }
    }

    if(bResultCorrect)
    {
        std::cout << "Execution results correct!" << std::endl;
    }
}
//-----------------------------------------------------------------------------
//! Program entry point.
//-----------------------------------------------------------------------------
int main()
{
    try
    {
        std::cout << std::endl;
        std::cout << "################################################################################" << std::endl;
        std::cout << "                              alpaka basic test                                 " << std::endl;
        std::cout << "################################################################################" << std::endl;
        std::cout << std::endl;

        // Logs the enabled accelerators.
        alpaka::logEnabledAccelerators();

        std::cout << std::endl;
		
        // Initialize the accelerators.
        alpaka::initAccelerators();

        // Set the grid size.
        alpaka::vec<3u> const v3uiSizeGridBlocks(16u, 8u, 4u);

        // Set the block size (to the minimum all enabled tests support).
        alpaka::vec<3u> const v3uiSizeBlockKernels(
#if defined ALPAKA_SERIAL_ENABLED
        1u, 1u, 1u
#elif defined ALPAKA_OPENMP_ENABLED
        4u, 4u, 2u
#elif defined ALPAKA_CUDA_ENABLED || defined ALPAKA_THREADS_ENABLED || defined ALPAKA_FIBERS_ENABLED
        16u, 16u, 2u
#else
        1u, 1u, 1u
#endif
        );

        using TuiNumUselessWork = boost::mpl::int_<100u>;
        std::uint32_t const uiMult2(5u);

        alpaka::WorkSize const workSize(v3uiSizeGridBlocks, v3uiSizeBlockKernels);

#ifdef ALPAKA_SERIAL_ENABLED
        std::cout << std::endl;
        std::cout << "################################################################################" << std::endl;
        profileAcceleratedExampleKernel<alpaka::AccSerial, TuiNumUselessWork>(workSize, uiMult2);
        std::cout << "################################################################################" << std::endl;
#endif
#ifdef ALPAKA_THREADS_ENABLED
        std::cout << std::endl;
        std::cout << "################################################################################" << std::endl;
        profileAcceleratedExampleKernel<alpaka::AccThreads, TuiNumUselessWork>(workSize, uiMult2);
        std::cout << "################################################################################" << std::endl;
#endif
#ifdef ALPAKA_FIBERS_ENABLED
        std::cout << std::endl;
        std::cout << "################################################################################" << std::endl;
        profileAcceleratedExampleKernel<alpaka::AccFibers, TuiNumUselessWork>(workSize, uiMult2);
        std::cout << "################################################################################" << std::endl;
#endif
#ifdef ALPAKA_OPENMP_ENABLED
        std::cout << std::endl;
        std::cout << "################################################################################" << std::endl;
        profileAcceleratedExampleKernel<alpaka::AccOpenMp, TuiNumUselessWork>(workSize, uiMult2);
        std::cout << "################################################################################" << std::endl;
#endif
#ifdef ALPAKA_CUDA_ENABLED
        std::cout << std::endl;
        std::cout << "################################################################################" << std::endl;
        profileAcceleratedExampleKernel<alpaka::AccCuda, TuiNumUselessWork>(workSize, uiMult2);
        std::cout << "################################################################################" << std::endl;
#endif
        std::cout << std::endl;

        return 0;
    }
    catch(std::exception const & e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    catch(...)
    {
        std::cerr << "Unknown Exception" << std::endl;
        return 1;
    }
}