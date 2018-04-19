/*
 * GpuModelSpecifics.hpp
 *
 *  Created on: Apr 4, 2016
 *      Author: msuchard
 */

#ifndef GPUMODELSPECIFICS_HPP_
#define GPUMODELSPECIFICS_HPP_


// #define USE_VECTOR
#undef USE_VECTOR

// #define GPU_DEBUG
#undef GPU_DEBUG
//#define USE_LOG_SUM
#define TIME_DEBUG

#include <Rcpp.h>

#include "ModelSpecifics.h"
#include "Iterators.h"

#include <boost/compute/algorithm/reduce.hpp>

namespace bsccs {

namespace compute = boost::compute;

namespace detail {

namespace constant {
    static const int updateXBetaBlockSize = 256; // 512; // Appears best on K40
    static const int updateAllXBetaBlockSize = 32;
}; // namespace constant

template <typename DeviceVec, typename HostVec>
DeviceVec allocateAndCopyToDevice(const HostVec& hostVec, const compute::context& context, compute::command_queue& queue) {
    DeviceVec deviceVec(hostVec.size(), context);
    compute::copy(std::begin(hostVec), std::end(hostVec), std::begin(deviceVec), queue);
    return std::move(deviceVec);
}

template <typename DeviceVec, typename HostVec>
void resizeAndCopyToDevice(const HostVec& hostVec, DeviceVec& deviceVec, compute::command_queue& queue) {
    deviceVec.resize(hostVec.size());
    compute::copy(std::begin(hostVec), std::end(hostVec), std::begin(deviceVec), queue);
}

template <typename HostVec, typename DeviceVec>
void compare(const HostVec& host, const DeviceVec& device, const std::string& error, double tolerance = 1E-10) {
    bool valid = true;

    for (size_t i = 0; i < host.size(); ++i) {
        auto h = host[i];
        auto d = device[i];
        if (std::abs(h - d) > tolerance) {
            std::cerr << "@ " << i << " : " << h << " - " << d << " = " << (h - d) << std::endl;
            valid = false;
        }
    }
    if (!valid) {
        //forward_exception_to_r(error);
        Rcpp::stop(error);
        // throw new std::logic_error(error);
    }
}

template <int D, class T>
int getAlignedLength(T n) {
    return (n / D) * D + (n % D == 0 ? 0 : D);
}

}; // namespace detail

struct SourceCode {
    std::string body;
    std::string name;

    SourceCode(std::string body, std::string name) : body(body), name(name) { }
};

template <typename RealType>
class AllGpuColumns {
public:
    typedef compute::vector<RealType> DataVector;
    typedef compute::vector<int> IndicesVector;
    typedef compute::uint_ UInt;
    typedef compute::vector<UInt> dStartsVector;
    typedef std::vector<UInt> hStartsVector;

    AllGpuColumns(const compute::context& context) : indices(context), data(context), // {
    		ddataStarts(context), dindicesStarts(context), dtaskCounts(context) {
        // Do nothing
    }

    virtual ~AllGpuColumns() { }

    void initialize(const CompressedDataMatrix& mat,
                    compute::command_queue& queue,
                    size_t K, bool pad) {
        std::vector<RealType> flatData;
        std::vector<int> flatIndices;

        std::cerr << "AGC start" << std::endl;

        UInt dataStart = 0;
        UInt indicesStart = 0;

        for (int j = 0; j < mat.getNumberOfColumns(); ++j) {
            const auto& column = mat.getColumn(j);
            const auto format = column.getFormatType();

            dataStarts.push_back(dataStart);
            indicesStarts.push_back(indicesStart);
            formats.push_back(format);

            // Data vector
            if (format == FormatType::SPARSE ||
                format == FormatType::DENSE) {
                appendAndPad(column.getDataVector(), flatData, dataStart, pad);
            }

            // Indices vector
            if (format == FormatType::INDICATOR ||
                format == FormatType::SPARSE) {
                appendAndPad(column.getColumnsVector(), flatIndices, indicesStart, pad);
            }

            // Task count
            if (format == FormatType::DENSE ||
                format == FormatType::INTERCEPT) {
                taskCounts.push_back(K);
            } else { // INDICATOR, SPARSE
                taskCounts.push_back(column.getNumberOfEntries());
            }
        }

        detail::resizeAndCopyToDevice(flatData, data, queue);
        detail::resizeAndCopyToDevice(flatIndices, indices, queue);
        detail::resizeAndCopyToDevice(dataStarts, ddataStarts, queue);
        detail::resizeAndCopyToDevice(indicesStarts, dindicesStarts, queue);
        detail::resizeAndCopyToDevice(taskCounts, dtaskCounts, queue);


    	std::cerr << "AGC end " << flatData.size() << " " << flatIndices.size() << std::endl;
    }

    UInt getDataOffset(int column) const {
        return dataStarts[column];
    }

    UInt getIndicesOffset(int column) const {
        return indicesStarts[column];
    }

    UInt getTaskCount(int column) const {
    	return taskCounts[column];
    }

    const DataVector& getData() const {
        return data;
    }

    const IndicesVector& getIndices() const {
        return indices;
    }

    const dStartsVector& getDataStarts() const {
    	return ddataStarts;
    }

    const dStartsVector& getIndicesStarts() const {
    	return dindicesStarts;
    }

    const dStartsVector& getTaskCounts() const {
    	return dtaskCounts;
    }

    const std::vector<FormatType> getFormat() const{
    	return formats;
    }


private:

    template <class T>
    void appendAndPad(const T& source, T& destination, UInt& length, bool pad) {
        for (auto x : source) {
            destination.push_back(x);
        }
        if (pad) {
            auto i = source.size();
            const auto end = detail::getAlignedLength<16>(i);
            for (; i < end; ++i) {
                destination.push_back(typename T::value_type());
            }
            length += end;
        } else {
            length += source.size();
        }
    }


    IndicesVector indices;
    DataVector data;
	hStartsVector taskCounts;
	hStartsVector dataStarts;
	hStartsVector indicesStarts;

	dStartsVector dtaskCounts;
	dStartsVector ddataStarts;
	dStartsVector dindicesStarts;

	//std::vector<UInt> taskCounts;
    //std::vector<UInt> dataStarts;
    //std::vector<UInt> indicesStarts;
    std::vector<FormatType> formats;
};

template <typename RealType>
class GpuColumn {
public:
    typedef compute::vector<RealType> DataVector;
    typedef compute::vector<int> IndicesVector;
    typedef compute::uint_ UInt;

    //GpuColumn(const GpuColumn<RealType>& copy);

    GpuColumn(const CompressedDataColumn& column,
              const compute::context& context,
              compute::command_queue& queue,
              size_t denseLength)
        : format(column.getFormatType()), indices(context), data(context) {

            // Data vector
            if (format == FormatType::SPARSE ||
                format == FormatType::DENSE) {
                const auto& columnData = column.getDataVector();
                detail::resizeAndCopyToDevice(columnData, data, queue);
            }

            // Indices vector
            if (format == FormatType::INDICATOR ||
                format == FormatType::SPARSE) {
                const auto& columnIndices = column.getColumnsVector();
                detail::resizeAndCopyToDevice(columnIndices, indices, queue);
            }

            // Task count
            if (format == FormatType::DENSE ||
                format == FormatType::INTERCEPT) {
                tasks = static_cast<UInt>(denseLength);
            } else { // INDICATOR, SPARSE
                tasks = static_cast<UInt>(column.getNumberOfEntries());
            }
        }

    virtual ~GpuColumn() { }

    const IndicesVector& getIndicesVector() const { return indices; }
    const DataVector& getDataVector() const { return data; }
    UInt getTaskCount() const { return tasks; }

private:
    FormatType format;
    IndicesVector indices;
    DataVector data;
    UInt tasks;
};


template <class BaseModel, typename WeightType, class BaseModelG>
class GpuModelSpecifics : public ModelSpecifics<BaseModel, WeightType>, BaseModelG {
public:

    using ModelSpecifics<BaseModel, WeightType>::modelData;
    using ModelSpecifics<BaseModel, WeightType>::offsExpXBeta;
    using ModelSpecifics<BaseModel, WeightType>::hXBeta;
    using ModelSpecifics<BaseModel, WeightType>::hY;
    using ModelSpecifics<BaseModel, WeightType>::hNtoK;
    using ModelSpecifics<BaseModel, WeightType>::hNWeight;
    using ModelSpecifics<BaseModel, WeightType>::hKWeight;
    using ModelSpecifics<BaseModel, WeightType>::hPid;
    using ModelSpecifics<BaseModel, WeightType>::hPidInternal;
    using ModelSpecifics<BaseModel, WeightType>::hOffs;
    using ModelSpecifics<BaseModel, WeightType>::denomPid;
    using ModelSpecifics<BaseModel, WeightType>::accDenomPid;
    using ModelSpecifics<BaseModel, WeightType>::accNumerPid;
    using ModelSpecifics<BaseModel, WeightType>::accNumerPid2;
    using ModelSpecifics<BaseModel, WeightType>::accReset;
    using ModelSpecifics<BaseModel, WeightType>::numerPid;
    using ModelSpecifics<BaseModel, WeightType>::numerPid2;
    using ModelSpecifics<BaseModel, WeightType>::hXjY;
    using ModelSpecifics<BaseModel, WeightType>::hXjX;
    using ModelSpecifics<BaseModel, WeightType>::sparseIndices;
    using ModelSpecifics<BaseModel, WeightType>::K;
    using ModelSpecifics<BaseModel, WeightType>::J;
    using ModelSpecifics<BaseModel, WeightType>::N;
    using ModelSpecifics<BaseModel, WeightType>::duration;
    //using ModelSpecifics<BaseModel, WeightType>::hBeta;
    using ModelSpecifics<BaseModel, WeightType>::algorithmType;
    using ModelSpecifics<BaseModel, WeightType>::norm;
    using ModelSpecifics<BaseModel, WeightType>::boundType;
    using ModelSpecifics<BaseModel, WeightType>::hXt;
    using ModelSpecifics<BaseModel, WeightType>::logLikelihoodFixedTerm;

    using ModelSpecifics<BaseModel, WeightType>::syncCV;
    using ModelSpecifics<BaseModel, WeightType>::syncCVFolds;

    using ModelSpecifics<BaseModel, WeightType>::hNWeightPool;
    using ModelSpecifics<BaseModel, WeightType>::hKWeightPool;
    using ModelSpecifics<BaseModel, WeightType>::accDenomPidPool;
    using ModelSpecifics<BaseModel, WeightType>::accNumerPid2Pool;
    using ModelSpecifics<BaseModel, WeightType>::accResetPool;
    using ModelSpecifics<BaseModel, WeightType>::hPidPool;
    using ModelSpecifics<BaseModel, WeightType>::hPidInternalPool;
    using ModelSpecifics<BaseModel, WeightType>::hXBetaPool;
    using ModelSpecifics<BaseModel, WeightType>::offsExpXBetaPool;
    using ModelSpecifics<BaseModel, WeightType>::denomPidPool;
    using ModelSpecifics<BaseModel, WeightType>::numerPidPool;
    using ModelSpecifics<BaseModel, WeightType>::numerPid2Pool;
    using ModelSpecifics<BaseModel, WeightType>::hXjYPool;
    using ModelSpecifics<BaseModel, WeightType>::hXjXPool;
    using ModelSpecifics<BaseModel, WeightType>::logLikelihoodFixedTermPool;
    using ModelSpecifics<BaseModel, WeightType>::normPool;

    const static int tpb = 128; // threads-per-block  // Appears best on K40
    const static int maxWgs = 2;  // work-group-size

    // const static int globalWorkSize = tpb * wgs;

    GpuModelSpecifics(const ModelData& input,
                      const std::string& deviceName)
    : ModelSpecifics<BaseModel,WeightType>(input),
      device(compute::system::find_device(deviceName)),
      ctx(device),
      queue(ctx, device
          , compute::command_queue::enable_profiling
      ),
      dColumns(ctx),
      dY(ctx), dBeta(ctx), dXBeta(ctx), dExpXBeta(ctx), dDenominator(ctx), dAccDenominator(ctx), dBuffer(ctx), dKWeight(ctx), dNWeight(ctx),
      dId(ctx), dNorm(ctx), dOffs(ctx), dFixBeta(ctx), dIntVector1(ctx), dIntVector2(ctx), dIntVector3(ctx), dIntVector4(ctx), dRealVector1(ctx), dRealVector2(ctx), dFirstRow(ctx),
      dBuffer1(ctx), dXMatrix(ctx), dExpXMatrix(ctx), dOverflow0(ctx), dOverflow1(ctx), dNtoK(ctx), dAllDelta(ctx), dColumnsXt(ctx),
	  dXBetaVector(ctx), dOffsExpXBetaVector(ctx), dDenomPidVector(ctx), dNWeightVector(ctx), dKWeightVector(ctx), dPidVector(ctx),
	  dAccDenomPidVector(ctx), dAccNumerPidVector(ctx), dAccNumerPid2Vector(ctx), dAccResetVector(ctx), dPidInternalVector(ctx), dNumerPidVector(ctx),
	  dNumerPid2Vector(ctx), dNormVector(ctx), dXjXVector(ctx), dXjYVector(ctx), dDeltaVector(ctx), dBoundVector(ctx), dPriorParams(ctx), dBetaVector(ctx), dAllZero(ctx), dDoneVector(ctx),
	  dXBetaKnown(false), hXBetaKnown(false){

        std::cerr << "ctor GpuModelSpecifics" << std::endl;

        // Get device ready to compute
        std::cerr << "Using: " << device.name() << std::endl;
    }

    virtual ~GpuModelSpecifics() {
        std::cerr << "dtor GpuModelSpecifics" << std::endl;
    }

    virtual void deviceInitialization() {
#ifdef TIME_DEBUG
        std::cerr << "start dI" << std::endl;
#endif
        //isNvidia = compute::detail::is_nvidia_device(queue.get_device());

        int need = 0;

        // Copy data
        dColumns.initialize(modelData, queue, K, true);
        //this->initializeMmXt();
        //dColumnsXt.initialize(*hXt, queue, K, true);
        formatList.resize(J);

        for (size_t j = 0; j < J /*modelData.getNumberOfColumns()*/; ++j) {

#ifdef TIME_DEBUG
          //  std::cerr << "dI " << j << std::endl;
#endif

            const auto& column = modelData.getColumn(j);
            // columns.emplace_back(GpuColumn<real>(column, ctx, queue, K));
            need |= (1 << column.getFormatType());

            indicesFormats[(FormatType)column.getFormatType()].push_back(j);
            formatList[j] = (FormatType)column.getFormatType();
        }
            // Rcpp::stop("done");

        std::vector<FormatType> neededFormatTypes;
        for (int t = 0; t < 4; ++t) {
            if (need & (1 << t)) {
                neededFormatTypes.push_back(static_cast<FormatType>(t));
            }
        }

        auto& inputY = modelData.getYVectorRef();
        detail::resizeAndCopyToDevice(inputY, dY, queue);

        // Internal buffers
        //detail::resizeAndCopyToDevice(hBeta, dBeta, queue);
        detail::resizeAndCopyToDevice(hXBeta, dXBeta, queue);  hXBetaKnown = true; dXBetaKnown = true;
        detail::resizeAndCopyToDevice(offsExpXBeta, dExpXBeta, queue);
        detail::resizeAndCopyToDevice(denomPid, dDenominator, queue);
        detail::resizeAndCopyToDevice(accDenomPid, dAccDenominator, queue);
        detail::resizeAndCopyToDevice(hPidInternal, dId, queue);
        detail::resizeAndCopyToDevice(hOffs, dOffs, queue);
        detail::resizeAndCopyToDevice(hKWeight, dKWeight, queue);
        detail::resizeAndCopyToDevice(hNWeight, dNWeight, queue);

        std::cerr << "Format types required: " << need << std::endl;

        buildAllKernels(neededFormatTypes);
        std::cout << "built all kernels \n";

        //printAllKernels(std::cerr);
    }

    // updates everything
    virtual void computeRemainingStatistics(bool useWeights) {

        //std::cerr << "GPU::cRS called" << std::endl;
    	if (syncCV) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif

        auto& kernel = kernelComputeRemainingStatisticsSync;
        kernel.set_arg(0, dXBetaVector);
        kernel.set_arg(1, dOffsExpXBetaVector);
        kernel.set_arg(2, dDenomPidVector);
        kernel.set_arg(3, dY);
        kernel.set_arg(4, dOffs);
        kernel.set_arg(5, dPidVector);
        kernel.set_arg(6, cvIndexStride);
        int cvBlock = 32;
        kernel.set_arg(7, cvBlock);
        kernel.set_arg(8, syncCVFolds);

        int loops = syncCVFolds / cvBlock;
        if (syncCVFolds % cvBlock != 0) {
        	loops++;
        }

        size_t globalWorkSize[2];
        globalWorkSize[0] = loops*cvBlock;
        globalWorkSize[1] = K;
        size_t localWorkSize[2];
        localWorkSize[0] = cvBlock;
        localWorkSize[1] = 1;
        size_t dim = 2;

        queue.enqueue_nd_range_kernel(kernel, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

        /*
        std::vector<real> blah;
        blah.resize(dOffsExpXBetaVector.size());
        compute::copy(std::begin(dOffsExpXBetaVector), std::end(dOffsExpXBetaVector), std::begin(blah), queue);
        std::cout << "offs0: ";
        for (auto x:blah) {
        	std::cout << x << " ";
        }
        std::cout << "\n";
        */

        /*
    	std::vector<int> foldIndices;
    	size_t count = 0;
    	for (int cvIndex = 0; cvIndex < syncCVFolds; ++cvIndex) {
    		foldIndices.push_back(cvIndex);
    	}

        // get kernel
        auto& kernel = kernelComputeRemainingStatisticsSync;

        // set kernel args
        size_t taskCount = cvIndexStride;
        int dK = K;
        kernel.set_arg(0, dK);
        kernel.set_arg(1, dXBetaVector);
        kernel.set_arg(2, dOffsExpXBetaVector);
        kernel.set_arg(3, dDenomPidVector);
        kernel.set_arg(4, dY);
        kernel.set_arg(5, dOffs);
        kernel.set_arg(6, dPidVector);
        kernel.set_arg(7, cvIndexStride);
        detail::resizeAndCopyToDevice(foldIndices, dIntVector1, queue);
        kernel.set_arg(8, dIntVector1);

        // set work size, no looping
        size_t workGroups = taskCount / detail::constant::updateXBetaBlockSize;
        if (taskCount % detail::constant::updateXBetaBlockSize != 0) {
        	++workGroups;
        }
        const size_t globalWorkSize = workGroups * syncCVFolds * detail::constant::updateXBetaBlockSize;
        int blockSize = workGroups * detail::constant::updateXBetaBlockSize;
        kernel.set_arg(9, blockSize);

        // run kernel
        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, detail::constant::updateXBetaBlockSize);
        queue.finish();
        */



#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        duration["compRSGSync          "] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();;
#endif
    	} else {

        hBuffer.resize(K);

    	//compute::copy(std::begin(offsExpXBeta), std::end(offsExpXBeta), std::begin(dExpXBeta), queue);
/*
 	    // Currently RS only computed on CPU and then copied
        if (BaseModel::likelihoodHasDenominator) {
            compute::copy(std::begin(dExpXBeta), std::end(dExpXBeta), std::begin(hBuffer), queue);
            //compute::copy(std::begin(dXBeta), std::end(dXBeta), std::begin(hXBeta), queue);
            //compute::copy(std::begin(dDenominator), std::end(dDenominator), std::begin(denomPid), queue);
        }
        */

        /*
        compute::copy(std::begin(dExpXBeta), std::end(dExpXBeta), std::begin(hBuffer), queue);
        std::cout << "dExpXBeta: " << hBuffer[0] << ' ' << hBuffer[1] << '\n';

        ModelSpecifics<BaseModel, WeightType>::computeRemainingStatistics(useWeights);
        std::cout << "after cRS offsExpXBeta: " << offsExpXBeta[0] << ' ' << offsExpXBeta[1] << '\n';
		*/

#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        /*
        if (algorithmType == AlgorithmType::MM) {
        	compute::copy(std::begin(hBeta), std::end(hBeta), std::begin(dBeta), queue);
        }
        */

        /*
        if (BaseModel::likelihoodHasDenominator) {
            compute::copy(std::begin(offsExpXBeta), std::end(offsExpXBeta), std::begin(dExpXBeta), queue);
            //compute::copy(std::begin(denomPid), std::end(denomPid), std::begin(dDenominator), queue);
        }
        */

        //compute::copy(std::begin(dDenominator), std::end(dDenominator), std::begin(hBuffer), queue);
        //std::cout << "before kernel dDenominator: " << hBuffer[0] << " " << hBuffer[1] << '\n';

        // get kernel
        auto& kernel = kernelComputeRemainingStatistics;

        // set kernel args
        const auto taskCount = K;
        int dK = K;
        kernel.set_arg(0, dK);
        kernel.set_arg(1, dXBeta);
        kernel.set_arg(2, dExpXBeta);
        kernel.set_arg(3, dDenominator);
        kernel.set_arg(4, dY);
        kernel.set_arg(5, dOffs);
        kernel.set_arg(6, dId);

        // set work size, no looping
        size_t workGroups = taskCount / detail::constant::updateXBetaBlockSize;
        if (taskCount % detail::constant::updateXBetaBlockSize != 0) {
        	++workGroups;
        }
        const size_t globalWorkSize = workGroups * detail::constant::updateXBetaBlockSize;

        // run kernel
        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, detail::constant::updateXBetaBlockSize);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        duration["compRSG          "] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();;
#endif
    	}
    }

    // for syncCV, does nothing right now because hacking LR
    virtual void computeRemainingStatistics(bool useWeights, std::vector<bool>& fixBeta) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        return;
    	std::vector<int> foldIndices;
    	size_t count = 0;
    	for (int cvIndex = 0; cvIndex < syncCVFolds; ++cvIndex) {
    		if (!fixBeta[cvIndex]) {
    			++count;
    			foldIndices.push_back(cvIndex);
    		}
    	}

    	if (count==0) {
    		return;
    	}

        // get kernel
        auto& kernel = kernelComputeRemainingStatisticsSync;

        // set kernel args
        size_t taskCount = cvIndexStride;
        int dK = K;
        kernel.set_arg(0, dK);
        kernel.set_arg(1, dXBetaVector);
        kernel.set_arg(2, dOffsExpXBetaVector);
        kernel.set_arg(3, dDenomPidVector);
        kernel.set_arg(4, dY);
        kernel.set_arg(5, dOffs);
        kernel.set_arg(6, dPidVector);
        kernel.set_arg(7, cvIndexStride);
        detail::resizeAndCopyToDevice(foldIndices, dIntVector1, queue);
        kernel.set_arg(8, dIntVector1);

        // set work size, no looping
        size_t workGroups = taskCount / detail::constant::updateXBetaBlockSize;
        if (taskCount % detail::constant::updateXBetaBlockSize != 0) {
        	++workGroups;
        }
        const size_t globalWorkSize = workGroups * count * detail::constant::updateXBetaBlockSize;
        int blockSize = workGroups * detail::constant::updateXBetaBlockSize;
        kernel.set_arg(9, blockSize);

        // run kernel
        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, detail::constant::updateXBetaBlockSize);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        duration["compRSGSync          "] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();;
#endif

    }

    // single ccd
    void computeGradientAndHessian(int index, double *ogradient, double *ohessian, bool useWeights) {

#ifdef GPU_DEBUG
        ModelSpecifics<BaseModel, WeightType>::computeGradientAndHessian(index, ogradient, ohessian, useWeights);
        std::cerr << *ogradient << " & " << *ohessian << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        FormatType formatType = modelData.getFormatType(index);
        double gradient = 0.0;
        double hessian = 0.0;

        /*
        if (!dXBetaKnown) {
        	//compute::copy(std::begin(hBeta), std::end(hBeta), std::begin(dBeta), queue);
            compute::copy(std::begin(hXBeta), std::end(hXBeta), std::begin(dXBeta), queue);
            dXBetaKnown = true;
        }
        */

        if (BaseModel::exactCLR) {
        	auto& kernel = (useWeights) ? // Double-dispatch
        	                            kernelGradientHessianWeighted[formatType] :
        	                            kernelGradientHessianNoWeight[formatType];
        	//std::cerr << "index: " << index << '\n';

        	/*
// 32 rows at a time
        	if (!initialized) {
    		    computeRemainingStatistics(true);
        		detail::resizeAndCopyToDevice(hNtoK, dNtoK, queue);
        		maxN = 0;
        		maxCases = 0;
        		for (int i = 0; i < N; ++i) {
        			int newN = hNtoK[i+1] - hNtoK[i];
        			int newC = hNWeight[i];
        			if (newN > maxN) maxN = newN;
        			if (newC > maxCases) maxCases = newC;
        		}
        		hFirstRow.resize(3*(maxN+1)*N);
        		for (int i = 0; i < N*(maxN+1); ++i) {
#ifdef USE_LOG_SUM
        			hFirstRow[3*i] = 0;
        			hFirstRow[3*i+1] = -INFINITY;
        			hFirstRow[3*i+2] = -INFINITY;
#else
        			hFirstRow[3*i] = 1;
        			hFirstRow[3*i+1] = 0;
        			hFirstRow[3*i+2] = 0;
#endif
        		}
            	detail::resizeAndCopyToDevice(hFirstRow, dFirstRow, queue);
            	detail::resizeAndCopyToDevice(hNWeight, dNWeight, queue);
            	hOverflow.resize(3*(maxN+1)*N);
            	for (int i = 0; i < 3*(maxN+1)*N; ++i) {
            	    hOverflow[i] = 1.0;
            	}
            	detail::resizeAndCopyToDevice(hOverflow, dOverflow0, queue);
            	dBuffer1.resize(1,queue);
            	//dRealVector2.resize(112*N);
            	initialized = true;
        	}
        	kernel.set_arg(0, dColumns.getDataStarts());
        	kernel.set_arg(1, dColumns.getIndicesStarts());
        	kernel.set_arg(2, dColumns.getTaskCounts());
        	kernel.set_arg(3, dNtoK);
        	kernel.set_arg(4, dBuffer1);
        	kernel.set_arg(5, dColumns.getData());
        	kernel.set_arg(6, dColumns.getIndices());
        	kernel.set_arg(7, dExpXBeta);
        	detail::resizeAndCopyToDevice(hNWeight, dNWeight, queue);
        	kernel.set_arg(8, dNWeight);
        	hBuffer.resize(3*N);
        	for (int i = 0; i < 3*N; ++i) {
        	    hBuffer[i] = 0;
        	}
        	detail::resizeAndCopyToDevice(hBuffer, dBuffer, queue);
        	kernel.set_arg(9, dBuffer);
        	detail::resizeAndCopyToDevice(hFirstRow, dFirstRow, queue);
        	kernel.set_arg(10, dFirstRow);
        	kernel.set_arg(11, (maxN+1)*3);
        	//kernel.set_arg(13, dRealVector1);
        	//kernel.set_arg(14, dRealVector2);
        	kernel.set_arg(13, dOverflow0);
        	kernel.set_arg(14, index);

        	size_t workGroups = maxCases / 32;
        	if (maxCases % 32 > 0) ++workGroups;
        	const size_t globalWorkSize = N * 32;

        	for (int i = 0; i < workGroups; ++i) {
        		//std::cerr << " run" << i;
        		kernel.set_arg(0, dColumns.getDataStarts());
        		kernel.set_arg(1, dColumns.getIndicesStarts());
        		kernel.set_arg(2, dColumns.getTaskCounts());
        		detail::resizeAndCopyToDevice(hNtoK, dNtoK, queue);
        		kernel.set_arg(3, dNtoK);
        		kernel.set_arg(4, dBuffer1);
        		kernel.set_arg(5, dColumns.getData());
        		kernel.set_arg(6, dColumns.getIndices());
        		kernel.set_arg(7, dExpXBeta);
        		detail::resizeAndCopyToDevice(hNWeight, dNWeight, queue);
        		kernel.set_arg(8, dNWeight);
            	detail::resizeAndCopyToDevice(hBuffer, dBuffer, queue);
        		kernel.set_arg(9, dBuffer);
        		kernel.set_arg(10, dFirstRow);
        		kernel.set_arg(11, (1+maxN)*3);
        		//kernel.set_arg(13, dRealVector1);
        		//kernel.set_arg(14, dRealVector2);
        		detail::resizeAndCopyToDevice(hOverflow, dOverflow0, queue);
            	kernel.set_arg(13, dOverflow0);
            	kernel.set_arg(14, index);
            	kernel.set_arg(12, i);
    	        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, 32);
    	        queue.finish();
        	}

        	hBuffer1.resize(3*N);
        	compute::copy(std::begin(dBuffer), std::end(dBuffer), std::begin(hBuffer1), queue);
        	for (int i = 0; i < N; ++i) {
#ifdef USE_LOG_SUM
        		gradient -= (real) -exp(hBuffer1[3*i+1]-hBuffer1[3*i]);
        		hessian -= (real) (exp(2*(hBuffer1[3*i+1]-hBuffer1[3*i]))-exp(hBuffer1[3*i+2]-hBuffer1[3*i]));
#else
        		gradient -= (real)(-hBuffer1[3*i+1]/hBuffer1[3*i]);
        		hessian -= (real)((hBuffer1[3*i+1]/hBuffer1[3*i]) * (hBuffer1[3*i+1]/hBuffer1[3*i]) - hBuffer1[3*i+2]/hBuffer1[3*i]);
#endif
        	}

*/

        	// 1 col at a time
        	if (!initialized) {
        		totalCases = 0;
        		for (int i=0; i < N; ++i) {
        			totalCases += hNWeight[i];
        		}
        		int temp = 0;
        		maxN = 0;
        		subjects.resize(N);
        		for (int i = 0; i < N; ++i) {
        			int newN = hNtoK[i+1] - hNtoK[i];
        			subjects[i] = newN;
        			if (newN > maxN) maxN = newN;
        		}

        		// indices vector
        		std::vector<int> hIndices;
        		hIndices.resize(3*(N+totalCases));
        		temp = 0;
        		for (int i=0; i < N; ++i) {
        			hIndices[temp] = 0;
        			hIndices[temp+1] = 0;
        			hIndices[temp+2] = 0;
        			temp += 3;
        			for (int j = 3; j < 3*(hNWeight[i]+1); ++j) {
        				hIndices[temp] = i+1;
        				++temp;
        			}
        		}
        		detail::resizeAndCopyToDevice(hIndices, dIntVector1, queue);

        		// constant vectors
        		std::vector<real> hVector1;
        		std::vector<real> hVector2;
        		hVector1.resize(3*(N+totalCases));
        		hVector2.resize(3*(N+totalCases));
        		for (int i=0; i < N+totalCases; ++i) {
        			hVector1[3*i] = 0;
        			hVector1[3*i+1] = 1;
        			hVector1[3*i+2] = 1;
        			hVector2[3*i] = 0;
        			hVector2[3*i+1] = 0;
        			hVector2[3*i+2] = 1;
        		}
        		detail::resizeAndCopyToDevice(hVector1, dRealVector1, queue);
        		detail::resizeAndCopyToDevice(hVector2, dRealVector2, queue);

        		// overflow vectors
        		std::vector<int> hOverflow;
        		hOverflow.resize(N+1);
        		for (int i=0; i < N+1; ++i) {
        			hOverflow[i] = 0;
        		}
        		detail::resizeAndCopyToDevice(hOverflow, dOverflow0, queue);
        		detail::resizeAndCopyToDevice(hOverflow, dOverflow1, queue);

        		detail::resizeAndCopyToDevice(hNtoK, dNtoK, queue);

            	// B0 and B1
        	    temp = 0;
            	hBuffer0.resize(3*(N+totalCases));
        	    for (int i=0; i < 3*(N+totalCases); ++i) {
        	    	hBuffer0[i] = 0;
        	    }
        	    for (int i=0; i < N; ++i) {
        	    	hBuffer0[temp] = 1;
        	        temp += 3*(hNWeight[i]+1);
        	    }

                compute::copy(std::begin(dExpXBeta), std::end(dExpXBeta), std::begin(offsExpXBeta), queue);
                GenericIterator x(modelData, index);
        	    auto expX = offsExpXBeta.begin();
        	    xMatrix.resize((N+1) * maxN);
        	    expXMatrix.resize((N+1) * maxN);
        	    for (int j = 0; j < maxN; ++j) {
        	    	xMatrix[j*(N+1)] = 0;
        	    	expXMatrix[j*(N+1)] = 0;
        	    }

        	    for (int i = 1; i < (N+1); ++i) {
        	        for (int j = 0; j < maxN; ++j) {
        	            if (j < subjects[i-1]) {
        	                xMatrix[j*(N+1) + i] = x.value();
        	                expXMatrix[j*(N+1) + i] = *expX;
        	                ++expX;
        	                ++x;
        	            } else {
        	                xMatrix[j*(N+1) + i] = 0;
        	                expXMatrix[j*(N+1) + i] = -1;
        	            }
        	        }
        	    }
    		    computeRemainingStatistics(true);

        		initialized = true;
        	}
    	    detail::resizeAndCopyToDevice(hBuffer0, dBuffer, queue);
    	    detail::resizeAndCopyToDevice(hBuffer0, dBuffer1, queue);
    	    kernel.set_arg(0, dBuffer);
    	    kernel.set_arg(1, dBuffer1);
        	kernel.set_arg(2, dIntVector1);
        	kernel.set_arg(5, dRealVector1);
        	kernel.set_arg(6, dRealVector2);
    	    int dN = N;
    	    kernel.set_arg(7, dN);
            if (dKWeight.size() == 0) {
                kernel.set_arg(9, 0);
            } else {
                kernel.set_arg(9, dKWeight); // TODO Only when dKWeight gets reallocated
            }
        	kernel.set_arg(10, dOverflow0);
        	kernel.set_arg(11, dOverflow1);


            // X and ExpX matrices
            compute::copy(std::begin(dExpXBeta), std::end(dExpXBeta), std::begin(offsExpXBeta), queue);
            GenericIterator x(modelData, index);
    	    auto expX = offsExpXBeta.begin();
    	    xMatrix.resize((N+1) * maxN);
    	    expXMatrix.resize((N+1) * maxN);
    	    for (int j = 0; j < maxN; ++j) {
    	    	xMatrix[j*(N+1)] = 0;
    	    	expXMatrix[j*(N+1)] = 0;
    	    }

    	    for (int i = 1; i < (N+1); ++i) {
    	        for (int j = 0; j < maxN; ++j) {
    	            if (j < subjects[i-1]) {
    	                xMatrix[j*(N+1) + i] = x.value();
    	                expXMatrix[j*(N+1) + i] = *expX;
    	                ++expX;
    	                ++x;
    	            } else {
    	                xMatrix[j*(N+1) + i] = 0;
    	                expXMatrix[j*(N+1) + i] = -1;
    	            }
    	        }
    	    }

    	    detail::resizeAndCopyToDevice(xMatrix, dXMatrix, queue);
    	    detail::resizeAndCopyToDevice(expXMatrix, dExpXMatrix, queue);
    	    kernel.set_arg(3, dXMatrix);
    	    kernel.set_arg(4, dExpXMatrix);


    	    //kernel.set_arg(3, dColumns.getData());
        	//kernel.set_arg(4, dExpXBeta);
        	//kernel.set_arg(13, dNtoK);
        	//kernel.set_arg(14, dColumns.getDataOffset(index));

    	    compute::uint_ taskCount = 3*(N+totalCases);
    	    size_t workGroups = taskCount / detail::constant::updateAllXBetaBlockSize;
    	    if (taskCount % detail::constant::updateAllXBetaBlockSize != 0) {
    	    	++workGroups;
    	    }
    	    const size_t globalWorkSize = workGroups * detail::constant::updateAllXBetaBlockSize;
    	    kernel.set_arg(12, taskCount);

    	    //kernel.set_arg(8, maxN);
	        //queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, detail::constant::updateXBetaBlockSize);
	        //queue.finish();
#ifdef CYCLOPS_DEBUG_TIMING
        auto end0 = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name0 = "compGradHessGArgs";
        duration[name0] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end0 - start).count();
#endif

    	    for (int i=0; i < maxN; ++i) {
    	    	//std::cout << "run: " << i << '\n';
        	    kernel.set_arg(0, dBuffer);
        	    kernel.set_arg(1, dBuffer1);
            	kernel.set_arg(2, dIntVector1);
        	    kernel.set_arg(3, dXMatrix);
        	    kernel.set_arg(4, dExpXMatrix);
            	kernel.set_arg(5, dRealVector1);
            	kernel.set_arg(6, dRealVector2);
        	    kernel.set_arg(7, dN);
                if (dKWeight.size() == 0) {
                    kernel.set_arg(9, 0);
                } else {
                    kernel.set_arg(9, dKWeight); // TODO Only when dKWeight gets reallocated
                }
            	kernel.set_arg(10, dOverflow0);
            	kernel.set_arg(11, dOverflow1);
        	    kernel.set_arg(12, taskCount);

        	    kernel.set_arg(8, i);
    	        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, detail::constant::updateAllXBetaBlockSize);
    	        queue.finish();
    	    }

    	    hBuffer1.resize(3*(N+totalCases));
    	    if (maxN%2 == 0) {
    	    	compute::copy(std::begin(dBuffer), std::end(dBuffer), std::begin(hBuffer1), queue);
    	    } else {
    	    	compute::copy(std::begin(dBuffer1), std::end(dBuffer1), std::begin(hBuffer1), queue);
    	    }

    	    int temp = 0;
    	    for (int i=0; i<N; ++i) {
    	    	temp += hNWeight[i]+1;
    	        //std::cout<<"new values" << i << ": " << hBuffer1[3*temp-3] <<" | "<< hBuffer1[3*temp-2] << " | " << hBuffer1[3*temp-1] << '\n';
    	    	gradient -= (real)(-hBuffer1[3*temp-2]/hBuffer1[3*temp-3]);
    	    	hessian -= (real)((hBuffer1[3*temp-2]/hBuffer1[3*temp-3]) * (hBuffer1[3*temp-2]/hBuffer1[3*temp-3]) - hBuffer1[3*temp-1]/hBuffer1[3*temp-3]);
    	    }


        } else {

        	if (!initialized) {
        		computeRemainingStatistics(true);
        		initialized = true;
        	}

        	auto& kernel = (useWeights) ? // Double-dispatch
        			kernelGradientHessianWeighted[formatType] :
					kernelGradientHessianNoWeight[formatType];

        	// auto& column = columns[index];
        	// const auto taskCount = column.getTaskCount();

        	const auto taskCount = dColumns.getTaskCount(index);

        	const auto wgs = maxWgs;
        	const auto globalWorkSize = tpb * wgs;

        	size_t loops = taskCount / globalWorkSize;
        	if (taskCount % globalWorkSize != 0) {
        		++loops;
        	}

        	// std::cerr << dBuffer.get_buffer() << std::endl;

        	//         kernel.set_arg(0, 0);
        	//         kernel.set_arg(1, 0);
        	//         kernel.set_arg(2, taskCount);
        	//
        	//         kernel.set_arg(3, column.getDataVector());
        	//         kernel.set_arg(4, column.getIndicesVector());

        	// set kernel args
        	kernel.set_arg(0, dColumns.getDataOffset(index));
        	kernel.set_arg(1, dColumns.getIndicesOffset(index));
        	kernel.set_arg(2, taskCount);

        	kernel.set_arg(3, dColumns.getData());
        	kernel.set_arg(4, dColumns.getIndices());
        	kernel.set_arg(5, dY);
        	kernel.set_arg(6, dXBeta);
        	kernel.set_arg(7, dExpXBeta);
        	kernel.set_arg(8, dDenominator);
#ifdef USE_VECTOR
        	if (dBuffer.size() < maxWgs) {
        		dBuffer.resize(maxWgs, queue);
        		//compute::fill(std::begin(dBuffer), std::end(dBuffer), 0.0, queue); // TODO Not needed
        		kernel.set_arg(9, dBuffer); // Can get reallocated.
        		hBuffer.resize(2 * maxWgs);
        	}
#else

        	//if (dBuffer.size() < 2 * maxWgs) {
        		dBuffer.resize(2 * maxWgs, queue);
        		//compute::fill(std::begin(dBuffer), std::end(dBuffer), 0.0, queue); // TODO Not needed
        		kernel.set_arg(9, dBuffer); // Can get reallocated.
        		hBuffer.resize(2 * maxWgs);
        	//}
#endif
        	kernel.set_arg(10, dId);
        	if (dKWeight.size() == 0) {
        		kernel.set_arg(11, 0);
        	} else {
        		kernel.set_arg(11, dKWeight); // TODO Only when dKWeight gets reallocated
        	}

        	//         std::cerr << "loop= " << loops << std::endl;
        	//         std::cerr << "n   = " << taskCount << std::endl;
        	//         std::cerr << "gWS = " << globalWorkSize << std::endl;
        	//         std::cerr << "tpb = " << tpb << std::endl;
        	//
        	// std::cerr << kernel.get_program().source() << std::endl;


        	//         compute::vector<real> tmpR(taskCount, ctx);
        	//         compute::vector<int> tmpI(taskCount, ctx);

        	queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
        	queue.finish();

        	//         for (int i = 0; i < wgs; ++i) {
        	//             std::cerr << ", " << dBuffer[i];
        	//         }
        	//         std::cerr << std::endl;

        	// Get result
#ifdef USE_VECTOR
        	compute::copy(std::begin(dBuffer), std::end(dBuffer), reinterpret_cast<compute::double2_ *>(hBuffer.data()), queue);

        	double gradient = 0.0;
        	double hessian = 0.0;

        	for (int i = 0; i < 2 * wgs; i += 2) { // TODO Use SSE
        		gradient += hBuffer[i + 0];
        		hessian  += hBuffer[i + 1];
        	}

        	if (BaseModel::precomputeGradient) { // Compile-time switch
        		gradient -= hXjY[index];
        	}

        	if (BaseModel::precomputeHessian) { // Compile-time switch
        		hessian += static_cast<real>(2.0) * hXjX[index];
        	}

        	*ogradient = gradient;
        	*ohessian = hessian;
#else
        	compute::copy(std::begin(dBuffer), std::end(dBuffer), std::begin(hBuffer), queue);

        	for (int i = 0; i < wgs; ++i) { // TODO Use SSE
        		gradient += hBuffer[i];
        		hessian  += hBuffer[i + wgs];
        	}
        }

        if (BaseModel::precomputeGradient) { // Compile-time switch
        	gradient -= hXjY[index];
        }

        if (BaseModel::precomputeHessian) { // Compile-time switch
        	hessian += static_cast<real>(2.0) * hXjX[index];
        }

        *ogradient = gradient;
        *ohessian = hessian;
#endif


#ifdef GPU_DEBUG
        std::cerr << gradient << " & " << hessian << std::endl << std::endl;
#endif // GPU_DEBUG

        //         for (auto x : dBuffer) {
        //             std::cerr << x << std::endl;
        //         }
        // //         for(int i = 0; i < wgs; ++i) {
        // //             std::cerr << dBuffer[i] << std::endl;
        // //         }
        //         std::cerr << (-hXjY[index]) << "  " << "0.0" << std::endl;
        //
        //
        //         Rcpp::stop("out");

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "compGradHessG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

    }

    // syncCV ccd, single index, not using
    void computeGradientAndHessian(int index, std::vector<priors::GradientHessian>& ghList, std::vector<bool>& fixBeta, bool useWeights) {

#ifdef GPU_DEBUG
        ModelSpecifics<BaseModel, WeightType>::computeGradientAndHessian(index, ogradient, ohessian, useWeights);
        std::cerr << *ogradient << " & " << *ohessian << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        FormatType formatType = modelData.getFormatType(index);

        /*
        if (!dXBetaKnown) {
        	//compute::copy(std::begin(hBeta), std::end(hBeta), std::begin(dBeta), queue);
            compute::copy(std::begin(hXBeta), std::end(hXBeta), std::begin(dXBeta), queue);
            dXBetaKnown = true;
        }
        */

        /*
        std::vector<int> foldIndices;
        size_t count = 0;
        for (int cvIndex = 0; cvIndex < syncCVFolds; ++cvIndex) {
        	if (!fixBeta[cvIndex]) {
        		++count;
        		foldIndices.push_back(cvIndex);
        	}
        }
        if (count == 0) {
        	return;
        }

        if (!initialized) {
        	std::vector<bool> fixBetaTemp(syncCVFolds,true);
        	computeRemainingStatistics(true, fixBetaTemp);
        	initialized = true;
        }

        auto& kernel = kernelGradientHessianSync[formatType];

        // auto& column = columns[index];
        // const auto taskCount = column.getTaskCount();

        const auto taskCount = dColumns.getTaskCount(index);

        //const auto wgs = maxWgs;

        size_t loops = taskCount / tpb;
        if (taskCount % tpb != 0) {
        	++loops;
        }

        int wgs = loops;

        const auto globalWorkSize = tpb * wgs * count;


        // std::cerr << dBuffer.get_buffer() << std::endl;

        //         kernel.set_arg(0, 0);
        //         kernel.set_arg(1, 0);
        //         kernel.set_arg(2, taskCount);
        //
        //         kernel.set_arg(3, column.getDataVector());
        //         kernel.set_arg(4, column.getIndicesVector());

        // set kernel args

        kernel.set_arg(0, dColumns.getDataOffset(index));
        kernel.set_arg(1, dColumns.getIndicesOffset(index));
        kernel.set_arg(2, taskCount);

        kernel.set_arg(3, dColumns.getData());
        kernel.set_arg(4, dColumns.getIndices());
        kernel.set_arg(5, dY);

        kernel.set_arg(6, dXBetaVector);
        kernel.set_arg(7, dOffsExpXBetaVector);
        kernel.set_arg(8, dDenomPidVector);

        if (hBuffer.size() < 2*wgs*count) {
        	hBuffer.resize(2*wgs*count);
        }
        if (dBuffer.size() < 2*wgs*count) {
        	dBuffer.resize(2*wgs*count);
        }
        //detail::resizeAndCopyToDevice(hBuffer, dBuffer, queue);
        //compute::fill(std::begin(dBuffer), std::end(dBuffer), 0.0, queue); // TODO Not needed
        kernel.set_arg(9, dBuffer); // Can get reallocated.
        //}

        kernel.set_arg(10, dPidVector);
        //kernel.set_arg(11, dKWeightVector);
#ifdef CYCLOPS_DEBUG_TIMING
        auto start1 = bsccs::chrono::steady_clock::now();
#endif
        int dK = K;
        kernel.set_arg(12, cvIndexStride);
        kernel.set_arg(13, tpb*wgs);
        if (dIntVector1.size() < count) {
        	dIntVector1.resize(count,queue);
        }
        compute::copy(std::begin(foldIndices), std::end(foldIndices), std::begin(dIntVector1), queue);
        //detail::resizeAndCopyToDevice(foldIndices, dIntVector1, queue);
        kernel.set_arg(14, dIntVector1);


        if (dKWeightVector.size() == 0) {
        	kernel.set_arg(11, 0);
        } else {
        	kernel.set_arg(11, dKWeightVector); // TODO Only when dKWeight gets reallocated
        }

#ifdef CYCLOPS_DEBUG_TIMING
        auto end1 = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name1 = "compGradHessSyncCVKernelArgsG" + getFormatTypeExtension(formatType) + " ";
        duration[name1] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end1 - start1).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        auto start0 = bsccs::chrono::steady_clock::now();
#endif

        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        auto end0 = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name0 = "compGradHessSyncCVKernelG" + getFormatTypeExtension(formatType) + " ";
        duration[name0] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end0 - start0).count();
#endif

        compute::copy(std::begin(dBuffer), std::begin(dBuffer)+2*wgs*count, std::begin(hBuffer), queue);

        for (int i = 0; i < count; i++) {
        	int cvIndex = foldIndices[i];
        	for (int j = 0; j < wgs; ++j) { // TODO Use SSE
        		ghList[cvIndex].first += hBuffer[i*wgs+j];
        		ghList[cvIndex].second += hBuffer[count*wgs+i*wgs+j];
        	}

        	if (BaseModel::precomputeGradient) { // Compile-time switch
        		ghList[cvIndex].first -= hXjYPool[cvIndex][index];
        	}

            if (BaseModel::precomputeHessian) { // Compile-time switch
            	ghList[cvIndex].second += static_cast<real>(2.0) * hXjXPool[cvIndex][index];
            }
        }
        */
#ifdef CYCLOPS_DEBUG_TIMING
        auto start1 = bsccs::chrono::steady_clock::now();
#endif
        int wgs = 64;

        auto& kernel = kernelGradientHessianSync[formatType];
        const auto taskCount = dColumns.getTaskCount(index);

        kernel.set_arg(0, dColumns.getDataOffset(index));
        kernel.set_arg(1, dColumns.getIndicesOffset(index));
        kernel.set_arg(2, taskCount);

        kernel.set_arg(3, dColumns.getData());
        kernel.set_arg(4, dColumns.getIndices());
        kernel.set_arg(5, dY);

        kernel.set_arg(6, dXBetaVector);
        kernel.set_arg(7, dOffsExpXBetaVector);
        kernel.set_arg(8, dDenomPidVector);

        if (hBuffer.size() < 2*wgs*cvIndexStride) {
        	hBuffer.resize(2*wgs*cvIndexStride);
        }
        if (dBuffer.size() < 2*wgs*cvIndexStride) {
        	dBuffer.resize(2*wgs*cvIndexStride);
        }
        kernel.set_arg(9, dBuffer); // Can get reallocated.

        kernel.set_arg(10, dPidVector);
        //kernel.set_arg(11, dKWeightVector);
        kernel.set_arg(12, cvIndexStride);
        int cvBlock = 32;
        if (syncCVFolds > 32) cvBlock = 64;
        if (syncCVFolds > 64) cvBlock = 128;
        kernel.set_arg(13, cvBlock);
        kernel.set_arg(14, syncCVFolds);

        if (dKWeightVector.size() == 0) {
        	kernel.set_arg(11, 0);
        } else {
        	kernel.set_arg(11, dKWeightVector); // TODO Only when dKWeight gets reallocated
        }

        int loops = syncCVFolds / cvBlock;
        if (syncCVFolds % cvBlock != 0) {
        	loops++;
        }

        if (taskCount < 64) {
        	wgs = 8;
        }

        size_t globalWorkSize[2];
        globalWorkSize[0] = loops*cvBlock;
        globalWorkSize[1] = wgs;
        size_t localWorkSize[2];
        localWorkSize[0] = cvBlock;
        localWorkSize[1] = 1;
        size_t dim = 2;

#ifdef CYCLOPS_DEBUG_TIMING
        auto end1 = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name1 = "compGradHessSyncCVKernelArgsG" + getFormatTypeExtension(formatType) + " ";
        duration[name1] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end1 - start1).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        auto start0 = bsccs::chrono::steady_clock::now();
#endif

        queue.enqueue_nd_range_kernel(kernel, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

        auto& kernel1 = kernelReduceCVBuffer;
        kernel1.set_arg(0, dBuffer);
        if (dBuffer1.size() < 2*syncCVFolds) {
        	dBuffer1.resize(2*syncCVFolds, queue);
        }
        kernel1.set_arg(1, dBuffer1);
        kernel1.set_arg(2, syncCVFolds);
        kernel1.set_arg(3, cvIndexStride);
        kernel1.set_arg(4, wgs);

        queue.enqueue_1d_range_kernel(kernel1, 0, syncCVFolds*tpb, tpb);
        queue.finish();

        /*
        compute::copy(std::begin(dBuffer), std::begin(dBuffer)+2*wgs*cvIndexStride, std::begin(hBuffer), queue);
        std::cout << "bufferIn: ";
        for (int i=0; i<2*wgs*cvIndexStride; i++) {
        	std::cout << hBuffer[i] << " ";
        }
        std::cout << "\n";
        */

#ifdef CYCLOPS_DEBUG_TIMING
        auto end0 = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name0 = "compGradHessSyncCVKernelG" + getFormatTypeExtension(formatType) + " ";
        duration[name0] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end0 - start0).count();
#endif
        compute::copy(std::begin(dBuffer1), std::begin(dBuffer1)+2*syncCVFolds, std::begin(hBuffer), queue);
/*
        std::cout << "bufferOut: ";
        for (int i=0; i<2*syncCVFolds; i++) {
        	std::cout << hBuffer[i] << " ";
        }
        std::cout << "\n";
        */
        for (int cvIndex = 0; cvIndex < syncCVFolds; cvIndex++) {
        	ghList[cvIndex].first = hBuffer[cvIndex];
        	ghList[cvIndex].second = hBuffer[cvIndex + syncCVFolds];

    		if (BaseModel::precomputeGradient) { // Compile-time switch
    			ghList[cvIndex].first -= hXjYPool[cvIndex][index];
    		}

    		if (BaseModel::precomputeHessian) { // Compile-time switch
    			ghList[cvIndex].second += static_cast<real>(2.0) * hXjXPool[cvIndex][index];
    		}
        }
/*
        compute::copy(std::begin(dBuffer), std::begin(dBuffer)+2*wgs*cvIndexStride, std::begin(hBuffer), queue);

        for (int cvIndex = 0; cvIndex < syncCVFolds; cvIndex++) {
        	for (int j = 0; j < wgs; ++j) { // TODO Use SSE
        		ghList[cvIndex].first += hBuffer[j*cvIndexStride+cvIndex];
        		ghList[cvIndex].second += hBuffer[(j+wgs)*cvIndexStride+cvIndex];
        	}

    		if (BaseModel::precomputeGradient) { // Compile-time switch
    			ghList[cvIndex].first -= hXjYPool[cvIndex][index];
    		}

    		if (BaseModel::precomputeHessian) { // Compile-time switch
    			ghList[cvIndex].second += static_cast<real>(2.0) * hXjXPool[cvIndex][index];
    		}
    	}
    	*/


#ifdef GPU_DEBUG
        std::cerr << gradient << " & " << hessian << std::endl << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "compGradHessSyncCVG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif


    }

    // syncCV ccd
    void computeGradientAndHessian(std::vector<GradientHessian>& ghList, const std::vector<std::pair<int,int>>& updateIndices) {

#ifdef GPU_DEBUG
        ModelSpecifics<BaseModel, WeightType>::computeGradientAndHessian(index, ogradient, ohessian, useWeights);
        std::cerr << *ogradient << " & " << *ohessian << std::endl;
#endif // GPU_DEBUG
        std::vector<int> indexList[4];
        std::vector<int> cvIndexList[4];
        std::vector<int> ogIndexList[4];

        for (int i=0; i<updateIndices.size(); i++) {
        	int index = updateIndices[i].first;
        	indexList[formatList[index]].push_back(index);
        	cvIndexList[formatList[index]].push_back(updateIndices[i].second);
        	ogIndexList[formatList[index]].push_back(i);
        }
        const auto wgs = maxWgs;
        auto useWeights = true;

        for (int i = FormatType::DENSE; i <= FormatType::INTERCEPT; ++i) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        	FormatType formatType = (FormatType)i;
        	const auto length = indexList[formatType].size();
        	if (length == 0) {
        		continue;
        	}

        	auto& kernel = kernelGradientHessianSync1[formatType];

        	kernel.set_arg(0, dColumns.getDataStarts());
        	kernel.set_arg(1, dColumns.getIndicesStarts());
        	kernel.set_arg(2, dColumns.getTaskCounts());
        	kernel.set_arg(3, dColumns.getData());
        	kernel.set_arg(4, dColumns.getIndices());
        	kernel.set_arg(5, dY);
        	kernel.set_arg(6, dXBetaVector);
        	kernel.set_arg(7, dOffsExpXBetaVector);
        	kernel.set_arg(8, dDenomPidVector);
        	//detail::resizeAndCopyToDevice(hBuffer0, dBuffer, queue);
            if (hBuffer.size() < 2*wgs*length) {
            	hBuffer.resize(2*wgs*length);
            }
            if (dBuffer.size() < 2*wgs*length) {
            	dBuffer.resize(2*wgs*length);
            }
        	kernel.set_arg(9, dBuffer); // Can get reallocated.
        	kernel.set_arg(10, dPidVector);
        	if (dKWeightVector.size() == 0) {
        		kernel.set_arg(11, 0);
        	} else {
        		kernel.set_arg(11, dKWeightVector); // TODO Only when dKWeight gets reallocated
        	}
        	kernel.set_arg(12, cvIndexStride);
        	kernel.set_arg(13, tpb*wgs);
        	kernel.set_arg(14, wgs);
        	detail::resizeAndCopyToDevice(indexList[i], dIntVector1, queue);
        	kernel.set_arg(15, dIntVector1);
        	detail::resizeAndCopyToDevice(cvIndexList[i], dIntVector2, queue);
        	kernel.set_arg(16, dIntVector2);

        	const auto globalWorkSize = tpb * wgs * length;

            queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
            queue.finish();

            compute::copy(std::begin(dBuffer), std::begin(dBuffer)+2*wgs*length, std::begin(hBuffer), queue);

            for (int k = 0; k < length; k++) {
            	int index = indexList[formatType][k];
            	int cvIndex = cvIndexList[formatType][k];

            	for (int j = 0; j < wgs; ++j) { // TODO Use SSE
            		ghList[ogIndexList[formatType][k]].first += hBuffer[k*wgs+j];
            		ghList[ogIndexList[formatType][k]].second  += hBuffer[k*wgs+j+length*wgs];
            		//ghList[ogIndexList[formatType][k]].first += hBuffer[j+2*wgs*k];
            		//ghList[ogIndexList[formatType][k]].second  += hBuffer[j + wgs+2*wgs*k];
            	}

            	if (BaseModel::precomputeGradient) { // Compile-time switch
            		ghList[ogIndexList[formatType][k]].first -= hXjYPool[cvIndex][index];
            	}

            	if (BaseModel::precomputeHessian) { // Compile-time switch
            		ghList[ogIndexList[formatType][k]].second += static_cast<real>(2.0) * hXjXPool[cvIndex][index];
            	}
            }

#ifdef GPU_DEBUG
            std::cerr << gradient << " & " << hessian << std::endl << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
            auto end = bsccs::chrono::steady_clock::now();
            ///////////////////////////"
            auto name = "compGradHessSyncCVG" + getFormatTypeExtension(formatType) + " ";
            duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
        }
    }

    // single mm
	virtual void computeMMGradientAndHessian(std::vector<GradientHessian>& ghList, const std::vector<bool>& fixBeta, bool useWeights) {

        /*
        if (!dXBetaKnown) {
        	//compute::copy(std::begin(hBeta), std::end(hBeta), std::begin(dBeta), queue);
            compute::copy(std::begin(hXBeta), std::end(hXBeta), std::begin(dXBeta), queue);
            dXBetaKnown = true;
        }
        */

        // initialize
        if (!initialized) {
	        this->initializeMM(boundType);
		    detail::resizeAndCopyToDevice(norm, dNorm, queue);

		    std::cerr << "\n";
		    computeRemainingStatistics(true);
	    	//kernel.set_arg(12, dNorm);

	        this->initializeMmXt();
	        dColumnsXt.initialize(*hXt, queue, K, true);

        	initialized = true;
        }

        std::vector<int> indexList[4];
        for (int i=0; i<J; i++) {
        	if (!fixBeta[i]) {
        	    indexList[formatList[i]].push_back(i);
        	}
        }

        const auto wgs = maxWgs;
        const auto globalWorkSize = tpb * wgs;
        //detail::resizeAndCopyToDevice(hBuffer0, dBuffer, queue);

        for (int i = FormatType::DENSE; i <= FormatType::INTERCEPT; ++i) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
            FormatType formatType = (FormatType)i;
            int length = indexList[i].size();
        	if (length == 0) {
        		continue;
        	}
        	 auto& kernel = (useWeights) ? // Double-dispatch
        	        		kernelGradientHessianMMWeighted[formatType] :
        					kernelGradientHessianMMNoWeight[formatType];

        	 kernel.set_arg(0, dColumns.getDataStarts());
        	 kernel.set_arg(1, dColumns.getIndicesStarts());
        	 kernel.set_arg(2, dColumns.getTaskCounts());
        	 kernel.set_arg(3, dColumns.getData());
        	 kernel.set_arg(4, dColumns.getIndices());
        	 kernel.set_arg(5, dY);
        	 kernel.set_arg(6, dXBeta);
        	 kernel.set_arg(7, dExpXBeta);
        	 kernel.set_arg(8, dDenominator);
        	 //detail::resizeAndCopyToDevice(hBuffer0, dBuffer, queue);
        	 dBuffer.resize(2*wgs*length,queue);
        	 hBuffer.resize(2*wgs*length);
        	 kernel.set_arg(9, dBuffer); // Can get reallocated.
        	 kernel.set_arg(10, dId);
        	 if (dKWeight.size() == 0) {
        		 kernel.set_arg(11, 0);
        	 } else {
        		 kernel.set_arg(11, dKWeight); // TODO Only when dKWeight gets reallocated
        	 }
        	 kernel.set_arg(12, dNorm);
        	 //int dJ = indicesFormats[formatType].size();
        	 kernel.set_arg(13, globalWorkSize);
        	 kernel.set_arg(14, wgs);
        	 detail::resizeAndCopyToDevice(indexList[i], dIntVector1, queue);
        	 kernel.set_arg(15, dIntVector1);

        	 // set work size; yes looping
        	 //const auto wgs = maxWgs;
        	 //const auto globalWorkSize = tpb * wgs;

        	 // run kernel
        	 queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize*length, tpb);
        	 queue.finish();

             compute::copy(std::begin(dBuffer), std::end(dBuffer), std::begin(hBuffer), queue);

             for (int k = 0; k < length; k++) {
             	int index = indexList[formatType][k];

             	for (int j = 0; j < wgs; ++j) { // TODO Use SSE
             		ghList[index].first += hBuffer[j+2*wgs*k];
             		ghList[index].second  += hBuffer[j + wgs+2*wgs*k];
             	}

             	if (BaseModel::precomputeGradient) { // Compile-time switch
             		ghList[index].first -= hXjY[index];
             	}

             	if (BaseModel::precomputeHessian) { // Compile-time switch
             		ghList[index].second += static_cast<real>(2.0) * hXjX[index];
             	}
             }
#ifdef CYCLOPS_DEBUG_TIMING
            auto end = bsccs::chrono::steady_clock::now();
            ///////////////////////////"
            auto name = "compGradHessMMG" + getFormatTypeExtension(formatType) + " ";
            duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
        }

	}

	// syncCV mm
    void computeMMGradientAndHessian(std::vector<GradientHessian>& ghList, const std::vector<std::pair<int,int>>& updateIndices) {

#ifdef GPU_DEBUG
        ModelSpecifics<BaseModel, WeightType>::computeGradientAndHessian(index, ogradient, ohessian, useWeights);
        std::cerr << *ogradient << " & " << *ohessian << std::endl;
#endif // GPU_DEBUG

        // initialize
        if (!initialized) {
        	this->initializeMM(boundType);
        	detail::resizeAndCopyToDevice(norm, dNorm, queue);

        	std::vector<real> hNormTemp;
        	int garbage;
        	for (int i=0; i<syncCVFolds; i++) {
        		//std::cout << "hNWeightPool size" << i << ": " << hNWeightPool[i].size() << "\n";
        		appendAndPad(normPool[i], hNormTemp, garbage, pad);
        	}
        	detail::resizeAndCopyToDevice(hNormTemp, dNormVector, queue);

        	this->initializeMmXt();
        	dColumnsXt.initialize(*hXt, queue, K, true);

        	initialized = true;
        }

        std::vector<int> indexList[4];
        std::vector<int> cvIndexList[4];
        std::vector<int> ogIndexList[4];

        for (int i=0; i<updateIndices.size(); i++) {
        	int index = updateIndices[i].first;
        	indexList[formatList[index]].push_back(index);
        	cvIndexList[formatList[index]].push_back(updateIndices[i].second);
        	ogIndexList[formatList[index]].push_back(i);
        }

        const int wgs = 1;
        auto useWeights = true;

        for (int i = FormatType::DENSE; i <= FormatType::INTERCEPT; ++i) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        	FormatType formatType = (FormatType)i;
        	const auto length = indexList[formatType].size();
        	if (length == 0) {
        		continue;
        	}
        	//std::cout << "formatType " << formatType << ": " << length << "\n";
        	auto& kernel = kernelGradientHessianMMSync[formatType];

        	kernel.set_arg(0, dColumns.getDataStarts());
        	kernel.set_arg(1, dColumns.getIndicesStarts());
        	kernel.set_arg(2, dColumns.getTaskCounts());
        	kernel.set_arg(3, dColumns.getData());
        	kernel.set_arg(4, dColumns.getIndices());
        	kernel.set_arg(5, dY);
        	kernel.set_arg(6, dXBetaVector);
        	kernel.set_arg(7, dOffsExpXBetaVector);
        	kernel.set_arg(8, dDenomPidVector);
        	//detail::resizeAndCopyToDevice(hBuffer0, dBuffer, queue);
            if (hBuffer.size() < 2*wgs*length) {
            	hBuffer.resize(2*wgs*length);
            }
            if (dBuffer.size() < 2*wgs*length) {
            	dBuffer.resize(2*wgs*length);
            }
        	kernel.set_arg(9, dBuffer); // Can get reallocated.
        	kernel.set_arg(10, dPidVector);
        	if (dKWeightVector.size() == 0) {
        		kernel.set_arg(11, 0);
        	} else {
        		kernel.set_arg(11, dKWeightVector); // TODO Only when dKWeight gets reallocated
        	}
        	kernel.set_arg(12, cvIndexStride);
        	kernel.set_arg(13, tpb*wgs);
        	kernel.set_arg(14, wgs);
        	detail::resizeAndCopyToDevice(indexList[i], dIntVector1, queue);
        	kernel.set_arg(15, dIntVector1);
        	detail::resizeAndCopyToDevice(cvIndexList[i], dIntVector2, queue);
        	kernel.set_arg(16, dIntVector2);
        	kernel.set_arg(17, dNormVector);

        	const auto globalWorkSize = tpb * wgs * length;

            queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
            queue.finish();

            compute::copy(std::begin(dBuffer), std::end(dBuffer), std::begin(hBuffer), queue);

            for (int k = 0; k < length; k++) {
            	int index = indexList[formatType][k];
            	int cvIndex = cvIndexList[formatType][k];

            	for (int j = 0; j < wgs; ++j) { // TODO Use SSE
            		ghList[ogIndexList[formatType][k]].first += hBuffer[j+2*wgs*k];
            		ghList[ogIndexList[formatType][k]].second  += hBuffer[j + wgs+2*wgs*k];
            	}

            	if (BaseModel::precomputeGradient) { // Compile-time switch
            		ghList[ogIndexList[formatType][k]].first -= hXjYPool[cvIndex][index];
            	}

            	if (BaseModel::precomputeHessian) { // Compile-time switch
            		ghList[ogIndexList[formatType][k]].second += static_cast<real>(2.0) * hXjXPool[cvIndex][index];
            	}
            }

#ifdef GPU_DEBUG
            std::cerr << gradient << " & " << hessian << std::endl << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
            auto end = bsccs::chrono::steady_clock::now();
            ///////////////////////////"
            auto name = "compGradHessMMSyncCVG" + getFormatTypeExtension(formatType) + " ";
            duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
        }

    }

    // greedy ccd
	virtual void computeAllGradientAndHessian(std::vector<GradientHessian>& gh, const std::vector<bool>& fixBeta, bool useWeights) {

        std::vector<int> indexList[4];
        std::vector<int> ogIndexList[4];

        int count = 0;

        for (int i=0; i<J; i++) {
        	if (!fixBeta[i]) {
        		indexList[formatList[i]].push_back(i);
        		ogIndexList[formatList[i]].push_back(i);
        		count++;
        	}
        }

        std::cout << "made indexList\n";

        if (count == 0) {
        	return;
        }

        const auto wgs = maxWgs;
        //const auto globalWorkSize = tpb * wgs;

        for (int i = FormatType::DENSE; i <= FormatType::INTERCEPT; ++i) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
            FormatType formatType = (FormatType)i;
            int length = indexList[i].size();
        	if (length == 0) {
        		continue;
        	}
        	 auto& kernel = (useWeights) ? // Double-dispatch
        	        		kernelGradientHessianAllWeighted[formatType] :
        					kernelGradientHessianAllNoWeight[formatType];

        	 kernel.set_arg(0, dColumns.getDataStarts());
        	 kernel.set_arg(1, dColumns.getIndicesStarts());
        	 kernel.set_arg(2, dColumns.getTaskCounts());
        	 kernel.set_arg(3, dColumns.getData());
        	 kernel.set_arg(4, dColumns.getIndices());
        	 kernel.set_arg(5, dY);
        	 kernel.set_arg(6, dXBeta);
        	 kernel.set_arg(7, dExpXBeta);
        	 kernel.set_arg(8, dDenominator);
        	 //detail::resizeAndCopyToDevice(hBuffer0, dBuffer, queue);
        	 if (dBuffer.size() < 2*wgs*length) {
        		 dBuffer.resize(2*wgs*length, queue);
        	 }
        	 kernel.set_arg(9, dBuffer); // Can get reallocated.
        	 kernel.set_arg(10, dId);
        	 if (dKWeight.size() == 0) {
        		 kernel.set_arg(11, 0);
        	 } else {
        		 kernel.set_arg(11, dKWeight); // TODO Only when dKWeight gets reallocated
        	 }
        	 kernel.set_arg(12, tpb*wgs);
        	 kernel.set_arg(13, wgs);
        	 detail::resizeAndCopyToDevice(indexList[formatType], dIntVector1, queue);
        	 kernel.set_arg(14, dIntVector1);


        	 // set work size; yes looping
        	 //const auto wgs = maxWgs;
        	 const auto globalWorkSize = tpb * wgs * length;

        	 // run kernel
        	 queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
        	 std::cout << "kernel launched\n";
        	 queue.finish();

        	 std::cout << "kernel finished\n";

        	 hBuffer.resize(2*wgs*length);
        	 compute::copy(std::begin(dBuffer), std::begin(dBuffer)+2*wgs*length, std::begin(hBuffer), queue);

        	 for (int j=0; j<length; j++) {
        		 int index = ogIndexList[formatType][j];
        		 for (int k=0; k<wgs; k++) {
        			 gh[index].first += hBuffer[j*wgs+k];
        			 gh[index].second += hBuffer[j*wgs+k+length*wgs];
        		 }
        		 if (BaseModel::precomputeGradient) { // Compile-time switch
        			 gh[index].first -= hXjY[index];
        		 }

        		 if (BaseModel::precomputeHessian) { // Compile-time switch
        			 gh[index].second += static_cast<real>(2.0) * hXjX[index];
        		 }
        	 }
#ifdef CYCLOPS_DEBUG_TIMING
            auto end = bsccs::chrono::steady_clock::now();
            ///////////////////////////"
            auto name = "compGradHessAllG" + getFormatTypeExtension(formatType) + " ";
            duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
        }

        /*
        if (!dXBetaKnown) {
        	//compute::copy(std::begin(hBeta), std::end(hBeta), std::begin(dBeta), queue);
            compute::copy(std::begin(hXBeta), std::end(hXBeta), std::begin(dXBeta), queue);
            dXBetaKnown = true;
        }
        */

/*
    	std::cerr << "dBuffer:";
    	for (auto x : dBuffer) {
    		std::cerr << " " << x;
    	}
        std::cerr << "\n";
        */
	}

	// single index
    virtual void updateXBeta(real realDelta, int index, bool useWeights) {
#ifdef GPU_DEBUG
        ModelSpecifics<BaseModel, WeightType>::updateXBeta(realDelta, index, useWeights);
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        // get kernel
        auto& kernel = kernelUpdateXBeta[modelData.getFormatType(index)];

        const auto taskCount = dColumns.getTaskCount(index);

        // set kernel args
        kernel.set_arg(0, dColumns.getDataOffset(index));
        kernel.set_arg(1, dColumns.getIndicesOffset(index));
        kernel.set_arg(2, taskCount);
        kernel.set_arg(3, realDelta);
        kernel.set_arg(4, dColumns.getData());
        kernel.set_arg(5, dColumns.getIndices());
        kernel.set_arg(6, dY);
        kernel.set_arg(7, dXBeta);
        kernel.set_arg(8, dExpXBeta);
        kernel.set_arg(9, dDenominator);
        kernel.set_arg(10, dId);

        // set work size; no looping
        size_t workGroups = taskCount / detail::constant::updateXBetaBlockSize;
        if (taskCount % detail::constant::updateXBetaBlockSize != 0) {
            ++workGroups;
        }
        const size_t globalWorkSize = workGroups * detail::constant::updateXBetaBlockSize;

        // run kernel
        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, detail::constant::updateXBetaBlockSize);
        queue.finish();

        hXBetaKnown = false; // dXBeta was just updated

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "updateXBetaG" + getFormatTypeExtension(modelData.getFormatType(index)) + "  ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef GPU_DEBUG
        // Compare results:
        detail::compare(hXBeta, dXBeta, "xBeta not equal");
        detail::compare(offsExpXBeta, dExpXBeta, "expXBeta not equal");
        detail::compare(denomPid, dDenominator, "denominator not equal");
#endif // GPU_DEBUG
    }

    // single index syncCV, not using
    virtual void updateXBeta(std::vector<double>& realDelta, int index, bool useWeights) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
/*
        std::vector<int> foldIndices;
        size_t count = 0;
        for (int cvIndex = 0; cvIndex < syncCVFolds; ++cvIndex) {
        	if (realDelta[cvIndex]!=0.0) {
        		++count;
        		foldIndices.push_back(cvIndex);
        	}
        }

        if (count==0) {
        	return;
        }

        // get kernel
        auto& kernel = kernelUpdateXBetaSync[modelData.getFormatType(index)];

        const auto taskCount = dColumns.getTaskCount(index);
        const auto localstride = detail::getAlignedLength<16>(taskCount);

        // set kernel args
        kernel.set_arg(0, dColumns.getDataOffset(index));
        kernel.set_arg(1, dColumns.getIndicesOffset(index));
        kernel.set_arg(2, taskCount);
        if (dRealVector1.size() < syncCVFolds) {
        	dRealVector1.resize(syncCVFolds, queue);
        }
        compute::copy(std::begin(realDelta), std::end(realDelta), std::begin(dRealVector1), queue);
        //detail::resizeAndCopyToDevice(realDelta, dRealVector1, queue);
        kernel.set_arg(3, dRealVector1);
        kernel.set_arg(4, dColumns.getData());
        kernel.set_arg(5, dColumns.getIndices());
        kernel.set_arg(6, dY);
        kernel.set_arg(7, dXBetaVector);
        kernel.set_arg(8, dOffsExpXBetaVector);
        kernel.set_arg(9, dDenomPidVector);
        kernel.set_arg(10, dPidVector);
        int dK = K;
        kernel.set_arg(11, cvIndexStride);
        if (dIntVector1.size() < count) {
        	dIntVector1.resize(count, queue);
        }
        compute::copy(std::begin(foldIndices), std::end(foldIndices), std::begin(dIntVector1), queue);
        //detail::resizeAndCopyToDevice(foldIndices, dIntVector1, queue);
        kernel.set_arg(12, dIntVector1);

        //kernel.set_arg(13, localstride);

        // set work size; no looping
        size_t workGroups = localstride / detail::constant::updateXBetaBlockSize;
        if (localstride % detail::constant::updateXBetaBlockSize != 0) {
            ++workGroups;
        }
        //int blah = workGroups;
        const size_t globalWorkSize = workGroups * count * detail::constant::updateXBetaBlockSize;
        int blockSize = workGroups * detail::constant::updateXBetaBlockSize;

        kernel.set_arg(13, blockSize);
        kernel.set_arg(14, dOffs);


#ifdef CYCLOPS_DEBUG_TIMING
        auto start0 = bsccs::chrono::steady_clock::now();
#endif

        // run kernel
        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, detail::constant::updateXBetaBlockSize);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        auto end0 = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name0 = "updateXBetaSyncCVKernelG" + getFormatTypeExtension(modelData.getFormatType(index)) + "  ";
        duration[name0] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end0 - start0).count();
#endif
*/

        size_t count = 0;
        for (int cvIndex = 0; cvIndex < syncCVFolds; ++cvIndex) {
        	if (realDelta[cvIndex]!=0.0) {
        		++count;
        	}
        }
        if (count==0) {
        	return;
        }

        // get kernel
        auto& kernel = kernelUpdateXBetaSync[modelData.getFormatType(index)];

        const auto taskCount = dColumns.getTaskCount(index);

        // set kernel args
        kernel.set_arg(0, dColumns.getDataOffset(index));
        kernel.set_arg(1, dColumns.getIndicesOffset(index));
        if (dRealVector1.size() < syncCVFolds) {
        	dRealVector1.resize(syncCVFolds, queue);
        }
        compute::copy(std::begin(realDelta), std::end(realDelta), std::begin(dRealVector1), queue);
        //detail::resizeAndCopyToDevice(realDelta, dRealVector1, queue);
        kernel.set_arg(2, dRealVector1);
        kernel.set_arg(3, dColumns.getData());
        kernel.set_arg(4, dColumns.getIndices());
        kernel.set_arg(5, dY);
        kernel.set_arg(6, dXBetaVector);
        kernel.set_arg(7, dOffsExpXBetaVector);
        kernel.set_arg(8, dDenomPidVector);
        kernel.set_arg(9, dPidVector);
        kernel.set_arg(10, cvIndexStride);
        kernel.set_arg(11, dOffs);
        int cvBlock = 32;
        kernel.set_arg(12, cvBlock);
        kernel.set_arg(13, syncCVFolds);

        int loops = syncCVFolds / cvBlock;
        if (syncCVFolds % cvBlock != 0) {
        	loops++;
        }

        size_t globalWorkSize[2];
        globalWorkSize[0] = loops*cvBlock;
        globalWorkSize[1] = taskCount;
        size_t localWorkSize[2];
        localWorkSize[0] = cvBlock;
        localWorkSize[1] = 1;
        size_t dim = 2;


#ifdef CYCLOPS_DEBUG_TIMING
        auto start0 = bsccs::chrono::steady_clock::now();
#endif

        // run kernel
        queue.enqueue_nd_range_kernel(kernel, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        auto end0 = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name0 = "updateXBetaSyncCVKernelG" + getFormatTypeExtension(modelData.getFormatType(index)) + "  ";
        duration[name0] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end0 - start0).count();
#endif

        hXBetaKnown = false; // dXBeta was just updated
/*
        std::vector<real> temp;
        temp.resize(K*cvIndexStride);
        compute::copy(std::begin(dXBetaVector), std::begin(dXBetaVector)+K*cvIndexStride, std::begin(temp), queue);
        std::cout << "xbeta: ";
        for (auto x:temp) {
        	std::cout << x << " ";
        }
        std::cout << "\n";
*/
#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "updateXBetaSyncCVG" + getFormatTypeExtension(modelData.getFormatType(index)) + "  ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
    }

    // syncCV ccd
    void updateXBeta(std::vector<double>& allDelta, std::vector<std::pair<int,int>>& updateIndices, bool useWeights) {
        std::vector<int> indexList[4];
        std::vector<int> cvIndexList[4];
        std::vector<int> ogIndexList[4];
        std::vector<real> deltaList[4];

        int count = 0;
        for (int i=0; i<updateIndices.size(); i++) {
        	if (allDelta[i] == 0.0) {
        		continue;
        	}
        	int index = updateIndices[i].first;
        	indexList[formatList[index]].push_back(index);
        	cvIndexList[formatList[index]].push_back(updateIndices[i].second);
        	ogIndexList[formatList[index]].push_back(i);
        	deltaList[formatList[index]].push_back((real)allDelta[i]);
        	count++;
        }

        if (count == 0) {
        	return;
        }

        const auto wgs = 4;

        for (int i = FormatType::DENSE; i <= FormatType::INTERCEPT; ++i) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        	FormatType formatType = (FormatType)i;
        	const auto length = indexList[i].size();
        	if (length == 0) {
        		continue;
        	}
        	// get kernel
        	auto& kernel = kernelUpdateXBetaSync1[formatType];

        	// set kernel args
        	kernel.set_arg(0, dColumns.getDataStarts());
        	kernel.set_arg(1, dColumns.getIndicesStarts());
        	kernel.set_arg(2, dColumns.getTaskCounts());
        	detail::resizeAndCopyToDevice(deltaList[i], dRealVector1, queue);
        	kernel.set_arg(3, dRealVector1);
        	kernel.set_arg(4, dColumns.getData());
        	kernel.set_arg(5, dColumns.getIndices());
        	kernel.set_arg(6, dY);
        	kernel.set_arg(7, dXBetaVector);
        	kernel.set_arg(8, dOffsExpXBetaVector);
        	kernel.set_arg(9, dDenomPidVector);
        	kernel.set_arg(10, dOffs);
        	kernel.set_arg(11, cvIndexStride);
        	kernel.set_arg(12, tpb*wgs);
        	kernel.set_arg(13, wgs);
        	detail::resizeAndCopyToDevice(indexList[i], dIntVector1, queue);
        	kernel.set_arg(14, dIntVector1);
        	detail::resizeAndCopyToDevice(cvIndexList[i], dIntVector2, queue);
        	kernel.set_arg(15, dIntVector2);

        	// set work size; no looping
        	const auto globalWorkSize = tpb * wgs * length;

        	queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
        	queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        	auto end = bsccs::chrono::steady_clock::now();
        	///////////////////////////"
        	auto name = "updateXBetaSyncCV1G" + getFormatTypeExtension(formatType) + "  ";
        	duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
        }
    	hXBetaKnown = false; // dXBeta was just updated
    }

    // syncCV mm
    void updateXBetaMM(std::vector<double>& allDelta, std::vector<std::pair<int,int>>& updateIndices, bool useWeights) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        std::vector<int> cvIndexList;
        std::vector<bool> cvIndexBool(syncCVFolds, false);

        int stride = J;
        if (pad) {
        	stride = detail::getAlignedLength<16>(J);
        }
        std::vector<real> deltaList(stride * syncCVFolds,0);

        int length = 0;
        for (int i=0; i<updateIndices.size(); i++) {
        	if (allDelta[i] == 0.0) {
        		continue;
        	}
        	int cvIndex = updateIndices[i].second;
        	cvIndexBool[cvIndex] = true;
        	deltaList[stride*cvIndex+updateIndices[i].first] = allDelta[i];
        	length++;
        }

        if (length == 0) {
        	return;
        }

        for (int i=0; i<syncCVFolds; i++) {
        	if (cvIndexBool[i]) {
        		cvIndexList.push_back(i);
        	}
        }

        const auto blockSize = 32;

        std::vector<int> blah;
        std::vector<real> blah2;

        auto& kernel = kernelUpdateXBetaMM;

        kernel.set_arg(0, dColumnsXt.getDataStarts());
        kernel.set_arg(1, dColumnsXt.getIndicesStarts());
        kernel.set_arg(2, dColumnsXt.getTaskCounts());
        kernel.set_arg(3, dColumnsXt.getData());
        kernel.set_arg(4, dColumnsXt.getIndices());
        kernel.set_arg(5, dY);
        kernel.set_arg(6, dXBetaVector);
        kernel.set_arg(7, dOffsExpXBetaVector);
        kernel.set_arg(8, dDenomPidVector);
        kernel.set_arg(9, dOffs);
        kernel.set_arg(10, stride);
        kernel.set_arg(11, cvIndexStride);
        detail::resizeAndCopyToDevice(deltaList, dRealVector1, queue);
        kernel.set_arg(12, dRealVector1);
        detail::resizeAndCopyToDevice(cvIndexList, dIntVector2, queue);
        kernel.set_arg(13, dIntVector2);
        int dK = K;
        kernel.set_arg(14, dK);

        const auto globalWorkSize = blockSize * K * cvIndexList.size();

        //std::cout << "globalWorkSize: " << globalWorkSize << "\n";

        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, blockSize);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        	auto end = bsccs::chrono::steady_clock::now();
        	///////////////////////////"
        	auto name = "updateXBetaMMSyncCVG";
        	duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
    	hXBetaKnown = false; // dXBeta was just updated
    }

    // less efficient syncCV mm
    /*
    void updateXBetaMM(std::vector<double>& allDelta, std::vector<std::pair<int,int>>& updateIndices, bool useWeights) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
    	std::vector<int> indexList;
        std::vector<int> cvIndexList;
        std::vector<real> deltaList;

        int length = 0;
        for (int i=0; i<updateIndices.size(); i++) {
        	if (allDelta[i] == 0.0) {
        		continue;
        	}
        	int index = updateIndices[i].first;
        	indexList.push_back(index);
        	cvIndexList.push_back(updateIndices[i].second);
        	deltaList.push_back((real)allDelta[i]);
        	length++;
        }

        if (length == 0) {
        	return;
        }

    	std::vector<int> cvIndices;
    	std::vector<int> cvLengths;
    	std::vector<int> cvOffsets;
    	int lastCvIndex = cvIndexList[0];
    	cvIndices.push_back(lastCvIndex);
    	cvOffsets.push_back(0);
    	int lastLength = 0;
    	for (int j=0; j<length; j++) {
    		int thisCvIndex = cvIndexList[j];
    		if (thisCvIndex > lastCvIndex) {
    			lastCvIndex = thisCvIndex;
    			cvIndices.push_back(lastCvIndex);
    			cvOffsets.push_back(j);
    			cvLengths.push_back(lastLength);
    			lastLength = 1;
    		} else {
    			lastLength++;
    		}
    	}
    	cvLengths.push_back(lastLength);

        const auto blockSize = 32;

        std::vector<int> blah;
        std::vector<real> blah2;

        auto& kernel = kernelUpdateXBetaMM;

        kernel.set_arg(0, dColumnsXt.getDataStarts());
        kernel.set_arg(1, dColumnsXt.getIndicesStarts());
        kernel.set_arg(2, dColumnsXt.getTaskCounts());
        kernel.set_arg(3, dColumnsXt.getData());
        kernel.set_arg(4, dColumnsXt.getIndices());
        kernel.set_arg(5, dY);
        kernel.set_arg(6, dXBetaVector);
        kernel.set_arg(7, dOffsExpXBetaVector);
        kernel.set_arg(8, dDenomPidVector);
        kernel.set_arg(9, dOffs);
        kernel.set_arg(10, cvIndexStride);
        detail::resizeAndCopyToDevice(deltaList, dRealVector1, queue);
        kernel.set_arg(11, dRealVector1);
        detail::resizeAndCopyToDevice(indexList, dIntVector1, queue);
        kernel.set_arg(12, dIntVector1);
        detail::resizeAndCopyToDevice(cvIndices, dIntVector2, queue);
        kernel.set_arg(13, dIntVector2);
        detail::resizeAndCopyToDevice(cvLengths, dIntVector3, queue);
        kernel.set_arg(14, dIntVector3);
        detail::resizeAndCopyToDevice(cvOffsets, dIntVector4, queue);
        kernel.set_arg(15, dIntVector4);
        int dK = K;
        kernel.set_arg(16, dK);

        const auto globalWorkSize = blockSize * K * cvIndices.size();

        //std::cout << "globalWorkSize: " << globalWorkSize << "\n";

        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, blockSize);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        	auto end = bsccs::chrono::steady_clock::now();
        	///////////////////////////"
        	auto name = "updateXBetaMMSyncCVG";
        	duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
    	hXBetaKnown = false; // dXBeta was just updated
    }
    */

    // single mm
    virtual void updateAllXBeta(std::vector<double>& allDelta, bool useWeights) {
#ifdef GPU_DEBUG
        ModelSpecifics<BaseModel, WeightType>::updateXBeta(realDelta, index, useWeights);
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif

        // get kernel
        auto& kernel = kernelUpdateAllXBeta[hXt->getFormatType(0)];

        // set kernel args
        kernel.set_arg(0, dColumnsXt.getDataStarts());
        kernel.set_arg(1, dColumnsXt.getIndicesStarts());
        kernel.set_arg(2, dColumnsXt.getTaskCounts());
        detail::resizeAndCopyToDevice(allDelta, dAllDelta, queue);
        kernel.set_arg(3, dAllDelta);
        kernel.set_arg(4, dColumnsXt.getData());
        kernel.set_arg(5, dColumnsXt.getIndices());
        kernel.set_arg(6, dY);
        kernel.set_arg(7, dXBeta);
    	const auto wgs = 1;
    	const auto globalWorkSize = detail::constant::updateAllXBetaBlockSize * wgs;
        kernel.set_arg(10, globalWorkSize);
        kernel.set_arg(11, wgs);
        std::vector<int> hFixBeta;
        hFixBeta.resize(J);
        for (int i=0; i<J; ++i) {
        	if (allDelta[i]==0) {
        		hFixBeta[i] = 1;
        	} else {
        		hFixBeta[i] = 0;
        	}
        	//hFixBeta[i] = fixBeta[i];
        }
        detail::resizeAndCopyToDevice(hFixBeta, dFixBeta, queue);
        kernel.set_arg(12, dFixBeta);

        /*
        	size_t workGroups = taskCount / detail::constant::updateXBetaBlockSize;
        	if (taskCount % detail::constant::updateXBetaBlockSize != 0) {
        		++workGroups;
        	}

        	const size_t globalWorkSize = workGroups * detail::constant::updateXBetaBlockSize;
         */
        // set work size; yes looping
        // run kernel
        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize*K, detail::constant::updateAllXBetaBlockSize);
        queue.finish();

        hXBetaKnown = false; // dXBeta was just updated

        /*
        hBuffer.resize(K);
        compute::copy(std::begin(dXBeta), std::end(dXBeta), std::begin(hBuffer), queue);
        std::cout << "hXBeta: ";
        for (auto x:hBuffer) {
        	std::cout << x << " ";
        }
        std::cout << "\n";
        */


#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "updateAllXBetaG" + getFormatTypeExtension(hXt->getFormatType(0)) + "  ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
    }

    // letting modelspecifics handle it, but kernel available
    virtual double getGradientObjective(bool useCrossValidation) {
#ifdef GPU_DEBUG
        ModelSpecifics<BaseModel, WeightType>::getGradientObjective(useCrossValidation);
        std::cerr << *ogradient << " & " << *ohessian << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif

        compute::copy(std::begin(dXBeta), std::end(dXBeta), std::begin(hXBeta), queue);
        return ModelSpecifics<BaseModel,WeightType>::getGradientObjective(useCrossValidation);
/*
        //FormatType formatType = FormatType::DENSE;

        auto& kernel = (useCrossValidation) ? // Double-dispatch
                            kernelGetGradientObjectiveWeighted :
							kernelGetGradientObjectiveNoWeight;

        const auto wgs = maxWgs;
        const auto globalWorkSize = tpb * wgs;

        int dK = K;
        kernel.set_arg(0, dK);
        kernel.set_arg(1, dY);
        kernel.set_arg(2, dXBeta);
        dBuffer.resize(2 * maxWgs, queue);
        kernel.set_arg(3, dBuffer); // Can get reallocated.
        hBuffer.resize(2 * maxWgs);
        if (dKWeight.size() == 0) {
            kernel.set_arg(4, 0);
        } else {
            kernel.set_arg(4, dKWeight); // TODO Only when dKWeight gets reallocated
        }

        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
        queue.finish();

        compute::copy(std::begin(dBuffer), std::end(dBuffer), std::begin(hBuffer), queue);

        double objective = 0.0;

        for (int i = 0; i < wgs; ++i) { // TODO Use SSE
        	objective += hBuffer[i];
        }

#ifdef GPU_DEBUG
        std::cerr << gradient << " & " << hessian << std::endl << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "compGradObj";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
        return(objective);
 */
    }

    // syncCV
    std::vector<double> getGradientObjectives() {
    	/*
    	for (int cvIndex=0; cvIndex<syncCVFolds; ++cvIndex) {
    		compute::copy(std::begin(dXBetaVector)+cvIndexStride*cvIndex, std::begin(dXBetaVector)+cvIndexStride*cvIndex+K, std::begin(hXBetaPool[cvIndex]), queue);
    	}
    	return ModelSpecifics<BaseModel,WeightType>::getGradientObjectives();
    	*/

#ifdef GPU_DEBUG
        ModelSpecifics<BaseModel, WeightType>::getGradientObjective(useCrossValidation);
        std::cerr << *ogradient << " & " << *ohessian << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif

        //FormatType formatType = FormatType::DENSE;
/*
        auto& kernel = kernelGetGradientObjectiveSync;

        const auto wgs = maxWgs;
        const auto globalWorkSize = tpb * wgs * syncCVFolds;

        int dK = K;
        kernel.set_arg(0, dK);
        kernel.set_arg(1, dY);
        kernel.set_arg(2, dXBetaVector);
        dBuffer.resize(wgs * syncCVFolds, queue);
        kernel.set_arg(3, dBuffer); // Can get reallocated.
        hBuffer.resize(wgs * syncCVFolds);
        kernel.set_arg(4, wgs*tpb);
        kernel.set_arg(5, wgs);
        kernel.set_arg(6, cvIndexStride);
        if (dKWeightVector.size() == 0) {
            kernel.set_arg(7, 0);
        } else {
            kernel.set_arg(7, dKWeightVector); // TODO Only when dKWeight gets reallocated
        }

        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
        queue.finish();

        compute::copy(std::begin(dBuffer), std::end(dBuffer), std::begin(hBuffer), queue);

        std::vector<double> result;
        result.resize(syncCVFolds, 0);
        for (int i=0; i<syncCVFolds; i++) {
        	for (int j = 0; j < wgs; ++j) { // TODO Use SSE
        		result[i] += hBuffer[i*wgs+j];
        	}
        }
        */
        auto& kernel = kernelGetGradientObjectiveSync;

        const auto wgs = 128;
        int dK = K;
        kernel.set_arg(0, dK);
        kernel.set_arg(1, dY);
        kernel.set_arg(2, dXBetaVector);
        if (dBuffer.size() < wgs*cvIndexStride) {
        	dBuffer.resize(wgs * cvIndexStride, queue);
        }
        if (hBuffer.size() < wgs*cvIndexStride) {
            hBuffer.resize(wgs * cvIndexStride);
        }
        kernel.set_arg(3, dBuffer); // Can get reallocated.
        kernel.set_arg(4, cvIndexStride);
        if (dKWeightVector.size() == 0) {
            kernel.set_arg(5, 0);
        } else {
            kernel.set_arg(5, dKWeightVector); // TODO Only when dKWeight gets reallocated
        }
        int cvBlock = 32;
        kernel.set_arg(6, cvBlock);
        kernel.set_arg(7, syncCVFolds);

        int loops = syncCVFolds / cvBlock;
        if (syncCVFolds % cvBlock != 0) {
        	loops++;
        }

        size_t globalWorkSize[2];
        globalWorkSize[0] = loops*cvBlock;
        globalWorkSize[1] = wgs;
        size_t localWorkSize[2];
        localWorkSize[0] = cvBlock;
        localWorkSize[1] = 1;
        size_t dim = 2;


        queue.enqueue_nd_range_kernel(kernel, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

        compute::copy(std::begin(dBuffer), std::begin(dBuffer)+wgs*cvIndexStride, std::begin(hBuffer), queue);

        std::vector<double> result;
        result.resize(syncCVFolds, 0.0);
        for (int cvIndex = 0; cvIndex < syncCVFolds; cvIndex++) {
        	for (int j = 0; j < wgs; ++j) { // TODO Use SSE
        		result[cvIndex] += hBuffer[j*cvIndexStride+cvIndex];
        	}
    	}

#ifdef GPU_DEBUG
        std::cerr << gradient << " & " << hessian << std::endl << std::endl;
#endif // GPU_DEBUG

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "compGradObjG";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

        return result;
    }

    // using kernel, should let modelspecifics handle maybe
    double getLogLikelihood(bool useCrossValidation) {

#ifdef CYCLOPS_DEBUG_TIMING
    	auto start = bsccs::chrono::steady_clock::now();
#endif
        auto& kernel = (useCrossValidation) ? // Double-dispatch
                            kernelGetLogLikelihoodWeighted :
							kernelGetLogLikelihoodNoWeight;

        const auto wgs = maxWgs;
        const auto globalWorkSize = tpb * wgs;

        int dK = K;
        int dN = N;
        // Run-time constant arguments.
        kernel.set_arg(0, dK);
        kernel.set_arg(1, dN);
        kernel.set_arg(2, dY);
        kernel.set_arg(3, dXBeta);
        kernel.set_arg(4, dDenominator);
        kernel.set_arg(5, dAccDenominator);
        dBuffer.resize(wgs, queue);
        kernel.set_arg(6, dBuffer); // Can get reallocated.
        hBuffer.resize(wgs);
        if (dKWeight.size() == 0) {
        	kernel.set_arg(7, 0);
        } else {
        	kernel.set_arg(7, dKWeight); // TODO Only when dKWeight gets reallocated
        }
        if (dNWeight.size() == 0) {
        	kernel.set_arg(7, 0);
        } else {
        	kernel.set_arg(8, dNWeight); // TODO Only when dKWeight gets reallocated
        }

        queue.enqueue_1d_range_kernel(kernel, 0, globalWorkSize, tpb);
        queue.finish();

        compute::copy(std::begin(dBuffer), std::end(dBuffer), std::begin(hBuffer), queue);

        double logLikelihood = 0.0;

        for (int i = 0; i < wgs; ++i) { // TODO Use SSE
        	logLikelihood += hBuffer[i];
        }

        if (BaseModel::likelihoodHasFixedTerms) {
        	logLikelihood += logLikelihoodFixedTerm;
        }

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        duration["compLogLikeG      "] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

        return(logLikelihood);
    }

    // syncCV, should write kernel
    std::vector<double> getLogLikelihoods(bool useCrossValidation) {
    	std::vector<real> xBetaTemp(dXBetaVector.size());
    	std::vector<real> denomTemp(dDenomPidVector.size());
		compute::copy(std::begin(dXBetaVector), std::end(dXBetaVector), std::begin(xBetaTemp), queue);
		compute::copy(std::begin(dDenomPidVector), std::end(dDenomPidVector), std::begin(denomTemp), queue);

		for (int i=0; i<K; i++) {
			for (int j=0; j<syncCVFolds; j++) {
				hXBetaPool[j][i] = xBetaTemp[i*cvIndexStride+j];
				denomPidPool[j][i] = denomTemp[i*cvIndexStride+j];
			}
		}

		/*
    	for (int cvIndex=0; cvIndex<syncCVFolds; ++cvIndex) {
    		compute::copy(std::begin(dXBetaVector)+cvIndexStride*cvIndex, std::begin(dXBetaVector)+cvIndexStride*cvIndex+K, std::begin(hXBetaPool[cvIndex]), queue);
    		compute::copy(std::begin(dDenomPidVector)+cvIndexStride*cvIndex, std::begin(dDenomPidVector)+cvIndexStride*cvIndex+K, std::begin(denomPidPool[cvIndex]), queue);
    		//compute::copy(std::begin(dAccDenomPidVector), std::begin(dAccDenomPidVector)+K, std::begin(accDenomPidPool[cvIndex]), queue);
    	}
    	*/
    	return ModelSpecifics<BaseModel,WeightType>::getLogLikelihoods(useCrossValidation);
    }

    // letting cpu handle, need to do pid for acc
    virtual double getPredictiveLogLikelihood(double* weights) {
        compute::copy(std::begin(dDenominator), std::end(dDenominator), std::begin(denomPid), queue);
        compute::copy(std::begin(dXBeta), std::end(dXBeta), std::begin(hXBeta), queue);
        compute::copy(std::begin(dKWeight), std::end(dKWeight), std::begin(hKWeight), queue);

        std::vector<real> saveKWeight;
        if(BaseModel::cumulativeGradientAndHessian)	{
        	saveKWeight = hKWeight; // make copy
        	// 		std::vector<int> savedPid = hPidInternal; // make copy
        	// 		std::vector<int> saveAccReset = accReset; // make copy
        	this->setPidForAccumulation(weights);
        	computeRemainingStatistics(true); // compute accDenomPid
        }

        // Compile-time switch for models with / with-out PID (hasIndependentRows)
        auto range = helper::getRangeAllPredictiveLikelihood(K, hY, hXBeta,
        		(BaseModel::cumulativeGradientAndHessian) ? accDenomPid : denomPid,
        				weights, hPid, std::integral_constant<bool, BaseModel::hasIndependentRows>());

        auto kernel = TestPredLikeKernel<BaseModel,real>();

        real logLikelihood = variants::reduce(
        		range.begin(), range.end(), static_cast<real>(0.0),
				kernel,
				SerialOnly()
        );

        if (BaseModel::cumulativeGradientAndHessian) {
        	// 		hPidInternal = savedPid; // make copy; TODO swap
        	// 		accReset = saveAccReset; // make copy; TODO swap
        	this->setPidForAccumulation(&saveKWeight[0]);
        	computeRemainingStatistics(true);
        }

        return static_cast<double>(logLikelihood);

    }   // END OF DIFF

    // letting cpu handle
    double getPredictiveLogLikelihood(double* weights, int cvIndex) {
    	std::vector<real> xBetaTemp(dXBetaVector.size());
    	std::vector<real> denomTemp(dDenomPidVector.size());
		compute::copy(std::begin(dXBetaVector), std::end(dXBetaVector), std::begin(xBetaTemp), queue);
		compute::copy(std::begin(dDenomPidVector), std::end(dDenomPidVector), std::begin(denomTemp), queue);

		for (int i=0; i<K; i++) {
			hXBetaPool[cvIndex][i] = xBetaTemp[i*cvIndexStride+cvIndex];
			denomPidPool[cvIndex][i] = denomTemp[i*cvIndexStride+cvIndex];
		}

    	//compute::copy(std::begin(dXBetaVector)+cvIndexStride*cvIndex, std::begin(dXBetaVector)+cvIndexStride*cvIndex+K, std::begin(hXBetaPool[cvIndex]), queue);
    	//compute::copy(std::begin(dDenomPidVector)+cvIndexStride*cvIndex, std::begin(dDenomPidVector)+cvIndexStride*cvIndex+K, std::begin(denomPidPool[cvIndex]), queue);
    	return ModelSpecifics<BaseModel, WeightType>::getPredictiveLogLikelihood(weights, cvIndex);
    }

    // letting cpu handle
    void getPredictiveEstimates(double* y, double* weights){

    	// TODO Check with SM: the following code appears to recompute hXBeta at large expense
    //	std::vector<real> xBeta(K,0.0);
    //	for(int j = 0; j < J; j++){
    //		GenericIterator it(modelData, j);
    //		for(; it; ++it){
    //			const int k = it.index();
    //			xBeta[k] += it.value() * hBeta[j] * weights[k];
    //		}
    //	}
        compute::copy(std::begin(dXBeta), std::end(dXBeta), std::begin(hXBeta), queue);
        ModelSpecifics<BaseModel, WeightType>::getPredictiveEstimates(y,weights);
    	// TODO How to remove code duplication above?
    }

    // set single weights
    virtual void setWeights(double* inWeights, bool useCrossValidation) {
    	// Currently only computed on CPU and then copied to GPU
    	ModelSpecifics<BaseModel, WeightType>::setWeights(inWeights, useCrossValidation);

    	detail::resizeAndCopyToDevice(hKWeight, dKWeight, queue);
    	detail::resizeAndCopyToDevice(hNWeight, dNWeight, queue);
    }

    // set syncCV weights
    virtual void setWeights(double* inWeights, bool useCrossValidation, int cvIndex) {
    	ModelSpecifics<BaseModel,WeightType>::setWeights(inWeights, useCrossValidation, cvIndex);
    	if (cvIndex == syncCVFolds - 1) {
    		//std::vector<real> hNWeightTemp;
    		//std::vector<real> hKWeightTemp;
    		//int garbage;
    		/*
    		for (int i=0; i<syncCVFolds; i++) {
    			//std::cout << "hNWeightPool size" << i << ": " << hNWeightPool[i].size() << "\n";
    			//appendAndPad(hNWeightPool[i], hNWeightTemp, garbage, pad); // not using for now
    			appendAndPad(hKWeightPool[i], hKWeightTemp, garbage, pad);
    		}
    		*/

    		std::vector<real> hKWeightTemp(K*cvIndexStride,0.0);
    		for (int i=0; i<K; i++) {
    			for (int j=0; j<syncCVFolds; j++) {
    				hKWeightTemp[i*cvIndexStride+j] = hKWeightPool[j][i];
    			}
    		}

    		//detail::resizeAndCopyToDevice(hNWeightTemp, dNWeightVector, queue);
    		detail::resizeAndCopyToDevice(hKWeightTemp, dKWeightVector, queue);
    	}
    }

    virtual const RealVector& getXBeta() {
        if (!hXBetaKnown) {
            compute::copy(std::begin(dXBeta), std::end(dXBeta), std::begin(hXBeta), queue);
            hXBetaKnown = true;
        }
        return ModelSpecifics<BaseModel,WeightType>::getXBeta();
    }

    virtual const RealVector& getXBetaSave() {
        return ModelSpecifics<BaseModel,WeightType>::getXBetaSave();
    }

    virtual void saveXBeta() {
        if (!hXBetaKnown) {
            compute::copy(std::begin(dXBeta), std::end(dXBeta), std::begin(hXBeta), queue);
            hXBetaKnown = true;
        }
        ModelSpecifics<BaseModel,WeightType>::saveXBeta();
    }

    virtual void zeroXBeta() {

        //std::cerr << "GPU::zXB called" << std::endl;

        ModelSpecifics<BaseModel,WeightType>::zeroXBeta(); // touches hXBeta
        /*
        if (syncCV) {
        	std::vector<real> temp;
        	int garbage;
        	for (int  i=0; i<syncCVFolds; i++) {
        		appendAndPad(hXBetaPool[i], temp, garbage, pad);
        	}
            detail::resizeAndCopyToDevice(temp, dXBetaVector, queue);
        } else {
            detail::resizeAndCopyToDevice(hXBeta, dXBeta, queue);
        }
        */
        if (syncCV) {
        	std::vector<real> temp(dXBetaVector.size(), 0.0);
        	compute::copy(std::begin(temp), std::end(temp), std::begin(dXBetaVector), queue);
        } else {
        	std::vector<real> temp(dXBeta.size(), 0.0);
        	compute::copy(std::begin(temp), std::end(temp), std::begin(dXBeta), queue);
        }

        //dXBetaKnown = false;
    }

    virtual void axpyXBeta(const double beta, const int j) {

        //std::cerr << "GPU::aXB called" << std::endl;

        ModelSpecifics<BaseModel,WeightType>::axpyXBeta(beta, j); // touches hXBeta
        detail::resizeAndCopyToDevice(hXBeta, dXBeta, queue);

        //dXBetaKnown = false;
    }

    virtual void axpyXBeta(const double beta, const int j, int cvIndex) {

    	ModelSpecifics<BaseModel,WeightType>::axpyXBeta(beta, j, cvIndex);
    	//compute::copy(std::begin(hXBetaPool[cvIndex]), std::end(hXBetaPool[cvIndex]), std::begin(dXBetaVector)+cvIndexStride * cvIndex, queue);
    }

    virtual void copyXBetaVec() {
    	std::vector<real> temp(K*cvIndexStride,0.0);
    	for (int i=0; i<K; i++) {
    		for (int j=0; j<syncCVFolds; j++) {
    			temp[i*cvIndexStride+j] = hXBetaPool[j][i];
    		}
    	}
    	/*
    	int garbage;
    	for (int  i=0; i<syncCVFolds; i++) {
    		appendAndPad(hXBetaPool[i], temp, garbage, pad);
    	}
    	*/
    	compute::copy(std::begin(temp), std::end(temp), std::begin(dXBetaVector), queue);
    	//detail::resizeAndCopyToDevice(temp, dXBetaVector, queue);
    }

    // not doing anything yet
    virtual void computeNumeratorForGradient(int index) {
    }

    // not doing anything yet
    virtual void computeNumeratorForGradient(int index, int cvIndex) {
    }

    virtual void resetBeta() {
    	std::vector<real> temp;
    	temp.resize(J*syncCVFolds, 0.0);
    	detail::resizeAndCopyToDevice(temp, dBetaVector, queue);
    }


    virtual void runCCDIndex() {
        for (int index = 0; index < J; index++) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        FormatType formatType = modelData.getFormatType(index);

        int wgs = 128;
        auto& kernel = kernelGradientHessianSync[formatType];
        const auto taskCount = dColumns.getTaskCount(index);

        kernel.set_arg(0, dColumns.getDataOffset(index));
        kernel.set_arg(1, dColumns.getIndicesOffset(index));
        kernel.set_arg(2, taskCount);
        kernel.set_arg(3, dColumns.getData());
        kernel.set_arg(4, dColumns.getIndices());
        kernel.set_arg(5, dY);
        kernel.set_arg(6, dXBetaVector);
        kernel.set_arg(7, dOffsExpXBetaVector);
        kernel.set_arg(8, dDenomPidVector);
        if (dBuffer.size() < 2*wgs*cvIndexStride) {
        	dBuffer.resize(2*wgs*cvIndexStride);
        }
        kernel.set_arg(9, dBuffer); // Can get reallocated.
        kernel.set_arg(10, dPidVector);
        if (dKWeightVector.size() == 0) {
        	kernel.set_arg(11, 0);
        } else {
        	kernel.set_arg(11, dKWeightVector); // TODO Only when dKWeight gets reallocated
        }

        kernel.set_arg(12, cvIndexStride);
        int cvBlock = 32;
        if (syncCVFolds > 32) cvBlock = 64;
        if (syncCVFolds > 64) cvBlock = 128;
        kernel.set_arg(13, cvBlock);
        kernel.set_arg(14, syncCVFolds);
        kernel.set_arg(15, dAllZero);

        int loops = syncCVFolds / cvBlock;
        if (syncCVFolds % cvBlock != 0) {
        	loops++;
        }

        size_t globalWorkSize[2];
        globalWorkSize[0] = loops*cvBlock;
        globalWorkSize[1] = wgs;
        size_t localWorkSize[2];
        localWorkSize[0] = cvBlock;
        localWorkSize[1] = 1;
        size_t dim = 2;

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "compGradHessArgsG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif

        queue.enqueue_nd_range_kernel(kernel, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compGradHessKernelG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif

        int priorType = priorTypes[index];
        auto& kernel1 = kernelProcessDeltaBuffer[priorType];

        kernel1.set_arg(0, dBuffer);
        if (dDeltaVector.size() < J*cvIndexStride) {
        	dDeltaVector.resize(J*cvIndexStride);
        }
        kernel1.set_arg(1, dDeltaVector);
        kernel1.set_arg(2, syncCVFolds);
        kernel1.set_arg(3, cvIndexStride);
        kernel1.set_arg(4, wgs);
        kernel1.set_arg(5, dBoundVector);
        kernel1.set_arg(6, dPriorParams);
        kernel1.set_arg(7, dXjYVector);
        int dJ = J;
        kernel1.set_arg(8, dJ);
        kernel1.set_arg(9, index);
        kernel1.set_arg(10, dBetaVector);
        kernel1.set_arg(11, dAllZero);
        kernel1.set_arg(12, dDoneVector);

#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compProcessDeltaArgsG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif

        queue.enqueue_1d_range_kernel(kernel1, 0, syncCVFolds*tpb, tpb);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compProcessDeltaKernelG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

/*
#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif
        std::vector<int> stopNow;
        stopNow.resize(1);
        compute::copy(std::begin(dAllZero), std::begin(dAllZero)+1, std::begin(stopNow), queue);

#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "checkingAllZero" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

        if (stopNow[0] == 1) {
        	return;
        }
*/

#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif
        auto& kernel2 = kernelUpdateXBetaSync[modelData.getFormatType(index)];

        // set kernel args
        kernel2.set_arg(0, dColumns.getDataOffset(index));
        kernel2.set_arg(1, dColumns.getIndicesOffset(index));
        kernel2.set_arg(2, dDeltaVector);
        kernel2.set_arg(3, dColumns.getData());
        kernel2.set_arg(4, dColumns.getIndices());
        kernel2.set_arg(5, dY);
        kernel2.set_arg(6, dXBetaVector);
        kernel2.set_arg(7, dOffsExpXBetaVector);
        kernel2.set_arg(8, dDenomPidVector);
        kernel2.set_arg(9, dPidVector);
        kernel2.set_arg(10, cvIndexStride);
        kernel2.set_arg(11, dOffs);
        cvBlock = 32;
        kernel2.set_arg(12, cvBlock);
        kernel2.set_arg(13, syncCVFolds);
        kernel2.set_arg(14, index);
        kernel2.set_arg(15, dAllZero);

        loops = syncCVFolds / cvBlock;
        if (syncCVFolds % cvBlock != 0) {
        	loops++;
        }

        globalWorkSize[0] = loops*cvBlock;
        globalWorkSize[1] = taskCount;
        localWorkSize[0] = cvBlock;
        localWorkSize[1] = 1;
        dim = 2;

#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compUpdateXBetaArgsG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif

        // run kernel
		queue.enqueue_nd_range_kernel(kernel2, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

        hXBetaKnown = false; // dXBeta was just updated
#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compUpdateXBetaKernelG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

        /*
#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif
        auto& kernel3 = kernelEmpty;

        // set kernel args
        int blah = 0;
        kernel3.set_arg(0, blah);

        loops = syncCVFolds / cvBlockSize;
        if (syncCVFolds % cvBlockSize != 0) {
            loops++;
        }

        globalWorkSize[0] = loops*cvBlockSize;
        globalWorkSize[1] = taskCount;
        localWorkSize[0] = cvBlockSize;
        localWorkSize[1] = 1;
        dim = 2;

#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compEmpetyArgsG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif

        // run kernel
        queue.enqueue_nd_range_kernel(kernel3, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

        hXBetaKnown = false; // dXBeta was just updated
#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compEmptyKernelG";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
         */
        }
    }

/*
    virtual void runCCDIndex(int index) {
#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        FormatType formatType = modelData.getFormatType(index);
        int priorType = priorTypes[index];

        auto& kernel = kernelDoItAll[formatType*3+priorType];
        const auto taskCount = dColumns.getTaskCount(index);

        kernel.set_arg(0, dColumns.getDataOffset(index));
        kernel.set_arg(1, dColumns.getIndicesOffset(index));
        kernel.set_arg(2, taskCount);
        kernel.set_arg(3, dColumns.getData());
        kernel.set_arg(4, dColumns.getIndices());
        kernel.set_arg(5, dY);
        kernel.set_arg(6, dOffs);
        kernel.set_arg(7, dXBetaVector);
        kernel.set_arg(8, dOffsExpXBetaVector);
        kernel.set_arg(9, dDenomPidVector);
        kernel.set_arg(10, dPidVector);
        if (dKWeightVector.size() == 0) {
        	kernel.set_arg(11, 0);
        } else {
        	kernel.set_arg(11, dKWeightVector); // TODO Only when dKWeight gets reallocated
        }
        kernel.set_arg(12, dBoundVector);
        kernel.set_arg(13, dPriorParams);
        kernel.set_arg(14, dXjYVector);
        kernel.set_arg(15, dBetaVector);
        kernel.set_arg(16, dDoneVector);
        kernel.set_arg(17, cvIndexStride);
        kernel.set_arg(18, syncCVFolds);
        int dJ = J;
        kernel.set_arg(19, dJ);
        kernel.set_arg(20, index);

        int loops = syncCVFolds / 16;
        if (syncCVFolds % 16 != 0) {
        	loops++;
        }

        size_t globalWorkSize[2];
        globalWorkSize[0] = loops*16;
        globalWorkSize[1] = 16;
        size_t localWorkSize[2];
        localWorkSize[0] = 16;
        localWorkSize[1] = 16;
        size_t dim = 2;

#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "compDoItAllArgsG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif

        queue.enqueue_nd_range_kernel(kernel, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compDoItAllKernelG" + getFormatTypeExtension(formatType) + " ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
    }
    */

    virtual void runMM() {
        std::cout << "running mm kernels\n";

        if (!initialized) {
        	this->initializeMM(boundType);
        	detail::resizeAndCopyToDevice(norm, dNorm, queue);

        	std::cout << "dNorm size: " << dNorm.size() <<  "\n";
        	this->initializeMmXt();
        	dColumnsXt.initialize(*hXt, queue, K, true);

        	initialized = true;

			for (int i=0; i<J; i++) {
				int formatType = formatList[i];
				int priorType = priorTypes[i];
				indexListWithPrior[formatType*3+priorType].push_back(i);
			}
/*
			std::vector<int> blah;
			blah.resize(dColumnsXt.getTaskCounts().size());
			compute::copy(std::begin(dColumnsXt.getTaskCounts()), std::end(dColumnsXt.getTaskCounts()), std::begin(blah), queue);
			std::cout << "task counts: ";
			for (auto x:blah) {
				std::cout << x << " ";
			}
			std::cout << "\n";

			blah.resize(dColumnsXt.getIndices().size());
			compute::copy(std::begin(dColumnsXt.getIndices()), std::end(dColumnsXt.getIndices()), std::begin(blah), queue);
			std::cout << "indices: ";
			for (auto x:blah) {
				std::cout << x << " ";
			}
			std::cout << "\n";
*/
        }

    	if (dDeltaVector.size() < J*cvIndexStride) {
    		dDeltaVector.resize(J*cvIndexStride);
    	}

        for (int i = FormatType::DENSE; i <= FormatType::INTERCEPT; ++i) {
        	for (int j = 0; j < 3; j++) {
#ifdef CYCLOPS_DEBUG_TIMING
        	auto start = bsccs::chrono::steady_clock::now();
#endif
        	FormatType formatType = (FormatType)i;

        	int length = indexListWithPrior[i*3+j].size();
        	if (length == 0) {
        		continue;
        	}
        	auto& kernel = kernelMMFindDelta[i*3+j];
        	//printKernel(kernel, std::cerr);

        	kernel.set_arg(0, dColumns.getDataStarts());
        	kernel.set_arg(1, dColumns.getIndicesStarts());
        	kernel.set_arg(2, dColumns.getTaskCounts());
        	kernel.set_arg(3, dColumns.getData());
        	kernel.set_arg(4, dColumns.getIndices());
        	kernel.set_arg(5, dY);
        	kernel.set_arg(6, dOffs);
        	kernel.set_arg(7, dXBetaVector);
        	kernel.set_arg(8, dOffsExpXBetaVector);
        	kernel.set_arg(9, dDenomPidVector);
        	kernel.set_arg(10, dPidVector);
        	if (dKWeightVector.size() == 0) {
        		kernel.set_arg(11, 0);
        	} else {
        		kernel.set_arg(11, dKWeightVector); // TODO Only when dKWeight gets reallocated
        	}
        	kernel.set_arg(12, dNorm);
        	kernel.set_arg(13, dBoundVector);
        	kernel.set_arg(14, dPriorParams);
        	kernel.set_arg(15, dXjYVector);
        	kernel.set_arg(16, dBetaVector);
        	kernel.set_arg(17, dDoneVector);
        	detail::resizeAndCopyToDevice(indexListWithPrior[i*3+j], dIntVector1, queue);
        	kernel.set_arg(18, dIntVector1);
        	kernel.set_arg(19, dDeltaVector);
        	kernel.set_arg(20, cvIndexStride);
        	kernel.set_arg(21, syncCVFolds);
        	int dJ = J;
        	kernel.set_arg(22, dJ);

            auto cvBlock = 32;
            auto idBlock = 8;
            int loops = syncCVFolds / 32;
            if (syncCVFolds % 32 != 0) {
            	loops++;
            }

            size_t globalWorkSize[2];
            globalWorkSize[0] = loops*cvBlock;
            globalWorkSize[1] = idBlock*length;
            size_t localWorkSize[2];
            localWorkSize[0] = cvBlock;
            localWorkSize[1] = idBlock;
            size_t dim = 2;


#ifdef CYCLOPS_DEBUG_TIMING
            auto end = bsccs::chrono::steady_clock::now();
            ///////////////////////////"
            auto name = "compMMFindDeltaArgsG" + getFormatTypeExtension(formatType) + " ";
            duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
            start = bsccs::chrono::steady_clock::now();
#endif

            queue.enqueue_nd_range_kernel(kernel, dim, 0, globalWorkSize, localWorkSize);
            queue.finish();

#ifdef CYCLOPS_DEBUG_TIMING
            end = bsccs::chrono::steady_clock::now();
            ///////////////////////////"
            name = "compMMFindDeltaKernelG" + getFormatTypeExtension(formatType) + " ";
            duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif
        	}
        }
/*
        std::cout << "delta0: ";
        std::vector<real> temp;
        temp.resize(syncCVFolds);
        compute::copy(std::begin(dDeltaVector), std::begin(dDeltaVector)+syncCVFolds, std::begin(temp), queue);
        for (auto x:temp) {
        	std::cout << x << " ";
        }
        std::cout << "\n";

        std::cout << "before hbeta0: ";
        temp.resize(syncCVFolds);
        compute::copy(std::begin(dXBetaVector), std::begin(dXBetaVector)+syncCVFolds, std::begin(temp), queue);
        for (auto x:temp) {
        	std::cout << x << " ";
        }
        std::cout << "\n";
        */

#ifdef CYCLOPS_DEBUG_TIMING
        auto start = bsccs::chrono::steady_clock::now();
#endif
        auto& kernel1 = kernelUpdateXBetaMM;

        kernel1.set_arg(0, dColumnsXt.getDataStarts());
        kernel1.set_arg(1, dColumnsXt.getIndicesStarts());
        kernel1.set_arg(2, dColumnsXt.getTaskCounts());
        kernel1.set_arg(3, dColumnsXt.getData());
        kernel1.set_arg(4, dColumnsXt.getIndices());
        kernel1.set_arg(5, dY);
        kernel1.set_arg(6, dXBetaVector);
        kernel1.set_arg(7, dOffsExpXBetaVector);
        kernel1.set_arg(8, dDenomPidVector);
        kernel1.set_arg(9, dOffs);
        kernel1.set_arg(10, cvIndexStride);
        kernel1.set_arg(11, dDeltaVector);
        kernel1.set_arg(12, syncCVFolds);

        auto cvBlock = 32;
        auto idBlock = 8;
        int dK = K;
        int loops = syncCVFolds / 32;
        if (syncCVFolds % 32 != 0) {
        	loops++;
        }

        size_t globalWorkSize[2];
        globalWorkSize[0] = loops*cvBlock;
        globalWorkSize[1] = idBlock*dK;
        size_t localWorkSize[2];
        localWorkSize[0] = cvBlock;
        localWorkSize[1] = idBlock;
        size_t dim = 2;

        //std::cout << "globalsize: " << loops*cvBlock << " " << idBlock * dK << " localsize: " << cvBlock << " " << idBlock << "\n";


#ifdef CYCLOPS_DEBUG_TIMING
        auto end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        auto name = "compMMUpdateXBetaArgsG ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

#ifdef CYCLOPS_DEBUG_TIMING
        start = bsccs::chrono::steady_clock::now();
#endif


        //printKernel(kernel1,std::cerr);

        queue.enqueue_nd_range_kernel(kernel1, dim, 0, globalWorkSize, localWorkSize);
        queue.finish();

/*
        std::cout << "after hbeta0: ";
        temp.resize(syncCVFolds);
        compute::copy(std::begin(dXBetaVector), std::end(dXBetaVector)+syncCVFolds, std::begin(temp), queue);
        for (auto x:temp) {
        	std::cout << x << " ";
        }
        std::cout << "\n";
*/

#ifdef CYCLOPS_DEBUG_TIMING
        end = bsccs::chrono::steady_clock::now();
        ///////////////////////////"
        name = "compMMUpdateXBetaKernelG ";
        duration[name] += bsccs::chrono::duration_cast<chrono::TimingUnits>(end - start).count();
#endif

    }

    void turnOnSyncCV(int foldToCompute) {
    	ModelSpecifics<BaseModel, WeightType>::turnOnSyncCV(foldToCompute);

    	syncCV = true;
    	pad = true;
    	syncCVFolds = foldToCompute;

        if (pad) {
        	cvBlockSize = 32;
        	if (syncCVFolds > 32) cvBlockSize = 64;
        	//if (syncCVFolds > 64 && syncCVFolds <= 128) cvBlockSize = 128;
        	//cvIndexStride = detail::getAlignedLength<16>(K);
        	if (syncCVFolds > 64) cvBlockSize = 128;
        	if (cvBlockSize == 32)  cvIndexStride = detail::getAlignedLength<32>(syncCVFolds);
        	if (cvBlockSize == 64)  cvIndexStride = detail::getAlignedLength<64>(syncCVFolds);
        	if (cvBlockSize == 128)  cvIndexStride = detail::getAlignedLength<128>(syncCVFolds);
        } else {
        	// do not use
        	// cvIndexStride = K;
        	cvIndexStride = syncCVFolds;
        }



        std::cout << "cvStride: " << cvIndexStride << "\n";

    	//int dataStart = 0;
    	int garbage = 0;

    	std::vector<real> blah(cvIndexStride, 0);
    	std::vector<int> blah1(cvIndexStride, 0);
        //std::vector<real> hNWeightTemp;
        std::vector<real> hKWeightTemp;
        //std::vector<real> accDenomPidTemp;
        //std::vector<real> accNumerPidTemp;
        //std::vector<real> accNumerPid2Temp;
        //std::vector<int> accResetTemp;
        std::vector<int> hPidTemp;
        //std::vector<int> hPidInternalTemp;
        std::vector<real> hXBetaTemp;
        std::vector<real> offsExpXBetaTemp;
        std::vector<real> denomPidTemp;
        //std::vector<real> numerPidTemp;
        //std::vector<real> numerPid2Temp;
        //std::vector<real> hXjYTemp;
        //std::vector<real> hXjXTemp;
        //std::vector<real> logLikelihoodFixedTermTemp;
        //std::vector<IndexVectorPtr> sparseIndicesTemp;
        //std::vector<real> normTemp;
        //std::vector<int> cvIndexOffsets;

        for (int i=0; i<K; i++) {
        	//std::fill(std::begin(blah), std::end(blah), static_cast<real>(hKWeight[i]));
        	//appendAndPad(blah, hKWeightTemp, garbage, pad);

        	std::fill(std::begin(blah), std::end(blah), hXBeta[i]);
        	appendAndPad(blah, hXBetaTemp, garbage, pad);

        	std::fill(std::begin(blah), std::end(blah), offsExpXBeta[i]);
        	appendAndPad(blah, offsExpXBetaTemp, garbage, pad);

        	std::fill(std::begin(blah), std::end(blah), denomPid[i]);
        	appendAndPad(blah, denomPidTemp, garbage, pad);

        	std::fill(std::begin(blah1), std::end(blah1), hPid[i]);
        	appendAndPad(blah1, hPidTemp, garbage, pad);
        }
/*
        for (int i=0; i<foldToCompute; ++i) {
        	//cvIndexOffsets.push_back(indices1);
        	//appendAndPad(hNWeight, hNWeightTemp, garbage, pad);
        	appendAndPad(hKWeight, hKWeightTemp, garbage, pad);
        	//appendAndPad(accDenomPid, accDenomPidTemp, garbage, true);
        	//appendAndPad(accNumerPid, accNumerPidTemp, garbage, true);
        	//appendAndPad(accNumerPid2, accNumerPid2Temp, garbage, true);
        	//appendAndPad(accReset, accResetTemp, garbage, true);
        	//appendAndPad(hPidInternal, hPidInternalTemp, garbage, true);
        	appendAndPad(hXBeta, hXBetaTemp, garbage, pad);
        	appendAndPad(offsExpXBeta, offsExpXBetaTemp, garbage, pad);
        	appendAndPad(denomPid, denomPidTemp, garbage, pad);
        	//appendAndPad(numerPid, numerPidTemp, garbage, true);
        	//appendAndPad(numerPid2, numerPid2Temp, garbage, true);
        	//appendAndPad(hXjY, hXjYTemp, garbage, false);
        	//appendAndPad(hXjX, hXjXTemp, garbage, false);
        	//appendAndPad(sparseIndices, sparseIndicesTemp, garbage, false);
        	//appendAndPad(norm, normTemp, garbage, false);
        }
        */

        /*
        for (int i=0; i<foldToCompute; ++i) {
        	for (int n=0; n<K; ++n) {
        		hPidTemp.push_back(hPid[n]);
        	}
        	for (int n=K; n<cvIndexStride; ++n) {
        		hPidTemp.push_back(-1);
        	}
        	//logLikelihoodFixedTermTemp.push_back(logLikelihoodFixedTerm);
        }
        */

        //detail::resizeAndCopyToDevice(hNWeightTemp, dNWeightVector, queue);
        //detail::resizeAndCopyToDevice(hKWeightTemp, dKWeightVector, queue);
        //detail::resizeAndCopyToDevice(accDenomPidTemp, dAccDenomPidVector, queue);
        //detail::resizeAndCopyToDevice(accNumerPidTemp, dAccNumerPidVector, queue);
        //detail::resizeAndCopyToDevice(accNumerPid2Temp, dAccNumerPid2Vector, queue);
        //detail::resizeAndCopyToDevice(accResetTemp, dAccResetVector, queue);
        detail::resizeAndCopyToDevice(hPidTemp, dPidVector, queue);
        //detail::resizeAndCopyToDevice(hPidInternalTemp, dPidInternalVector, queue);
        detail::resizeAndCopyToDevice(hXBetaTemp, dXBetaVector, queue);
        detail::resizeAndCopyToDevice(offsExpXBetaTemp, dOffsExpXBetaVector, queue);
        detail::resizeAndCopyToDevice(denomPidTemp, dDenomPidVector, queue);
        //detail::resizeAndCopyToDevice(numerPidTemp, dNumerPidVector, queue);
        //detail::resizeAndCopyToDevice(numerPid2Temp, dNumerPid2Vector, queue);
        //detail::resizeAndCopyToDevice(hXjYTemp, dXjYVector, queue);
        //detail::resizeAndCopyToDevice(hXjXTemp, dXjXVector, queue);
        //detail::resizeAndCopyToDevice(logLikelihoodFixedTermTemp, dLogLikelihoodFixedTermVector, queue);
        //detail::resizeAndCopyToDevice(sparseIndicesTemp, dSpareIndicesVector, queue);
        //detail::resizeAndCopyToDevice(normTemp, dNormVector, queue);
        //detail::resizeAndCopyToDevice(cvIndexOffsets, dcvIndexOffsets, queue);

        std::vector<int> hAllZero;
        hAllZero.push_back(0);
        detail::resizeAndCopyToDevice(hAllZero, dAllZero, queue);

        std::vector<int> hDone;
        hDone.resize(syncCVFolds, 1);
        detail::resizeAndCopyToDevice(hDone, dDoneVector, queue);

        int need = 0;
        for (size_t j = 0; j < J /*modelData.getNumberOfColumns()*/; ++j) {
            const auto& column = modelData.getColumn(j);
            // columns.emplace_back(GpuColumn<real>(column, ctx, queue, K));
            need |= (1 << column.getFormatType());
        }
        std::vector<FormatType> neededFormatTypes;
        for (int t = 0; t < 4; ++t) {
            if (need & (1 << t)) {
                neededFormatTypes.push_back(static_cast<FormatType>(t));
            }
        }
        std::cerr << "Format types required: " << need << std::endl;
        buildAllSyncCVKernels(neededFormatTypes);
        std::cout << "built all syncCV kernels \n";

        //printAllSyncCVKernels(std::cerr);
    }

    void turnOffSyncCV() {
    	syncCV = false;
    }

    bool isGPU() {return true;};

    virtual void updateDoneFolds(std::vector<bool>& donePool) {
    	std::vector<int> temp;
    	temp.resize(syncCVFolds, 1);
    	for (int i=0; i<syncCVFolds; i++) {
    		if (donePool[i]) temp[i] = 0;
    	}
    	compute::copy(std::begin(temp), std::end(temp), std::begin(dDoneVector), queue);
    }

    void computeFixedTermsInGradientAndHessian(bool useCrossValidation) {
    	ModelSpecifics<BaseModel,WeightType>::computeFixedTermsInGradientAndHessian(useCrossValidation);

    	//std::vector<real> xjxTemp;
    	//xjxTemp.resize(J*syncCVFolds);
    	std::vector<real> xjyTemp;
    	xjyTemp.resize(J*syncCVFolds, 0.0);

    	for (int i=0; i<syncCVFolds; i++) {
    		for (int j=0; j<J; j++) {
    			//xjxTemp[i*J+j] = hXjXPool[i][j];
    			xjyTemp[i*J+j] = hXjYPool[i][j];
    		}
    	}

    	//detail::resizeAndCopyToDevice(xjxTemp, dXjXVector, queue);
    	detail::resizeAndCopyToDevice(xjyTemp, dXjYVector, queue);

    }

    void setBounds(double initialBound) {
    	std::vector<real> temp;
        temp.resize(J*syncCVFolds, initialBound);
    	detail::resizeAndCopyToDevice(temp, dBoundVector, queue);
    }

    void setPriorTypes(std::vector<int>& inTypes) {
    	priorTypes.resize(J);
    	for (int i=0; i<J; i++) {
    		priorTypes[i] = inTypes[i];
    	}
    }

    void setPriorParams(std::vector<double>& inParams) {
    	std::vector<real> temp;
    	temp.resize(J, 0.0);
    	for (int i=0; i<J; i++) {
    		temp[i] = inParams[i];
    	}
    	/*
    	std::cout << "prior types: ";
    	for (auto x:priorTypes) {
    		std::cout << x << " ";
    	}
    	std::cout << "\n";
    	std::cout << "setting prior params: ";
    	for (auto x:temp) {
    		std::cout << x << " ";
    	}
    	std::cout << "\n";
*/
    	detail::resizeAndCopyToDevice(temp, dPriorParams, queue);
    }


    private:

    template <class T>
    void appendAndPad(const T& source, T& destination, int& length, bool pad) {
        for (auto x : source) {
            destination.push_back(x);
        }
        if (pad) {
            auto i = source.size();
            const auto end = detail::getAlignedLength<16>(i);
            for (; i < end; ++i) {
                destination.push_back(typename T::value_type());
            }
            length += end;
        } else {
            length += source.size();
        }
    }

    void buildAllUpdateXBetaKernels(const std::vector<FormatType>& neededFormatTypes) {
        for (FormatType formatType : neededFormatTypes) {
            buildUpdateXBetaKernel(formatType);
            buildUpdateAllXBetaKernel(formatType);
        }
    }

    void buildAllSyncCVUpdateXBetaKernels(const std::vector<FormatType>& neededFormatTypes) {
        for (FormatType formatType : neededFormatTypes) {
            buildSyncCVUpdateXBetaKernel(formatType);
        }
        buildUpdateXBetaMMKernel();
    }

    void buildAllComputeRemainingStatisticsKernels() {
    	//for (FormatType formatType : neededFormatTypes) {
    		buildComputeRemainingStatisticsKernel();
    	//}
    }

    void buildAllSyncCVComputeRemainingStatisticsKernels() {
        buildSyncCVComputeRemainingStatisticsKernel();
    }

    void buildAllGradientHessianKernels(const std::vector<FormatType>& neededFormatTypes) {
        int b = 0;
        for (FormatType formatType : neededFormatTypes) {
            buildGradientHessianKernel(formatType, true); ++b;
            buildGradientHessianKernel(formatType, false); ++b;
        }
    }

    void buildAllSyncCVGradientHessianKernels(const std::vector<FormatType>& neededFormatTypes) {
        int b = 0;
        for (FormatType formatType : neededFormatTypes) {
            buildSyncCVGradientHessianKernel(formatType); ++b;
        }
    }

    void buildAllProcessDeltaKernels() {
    	buildProcessDeltaKernel(0);
    	buildProcessDeltaKernel(1);
    	buildProcessDeltaKernel(2);
    }

    void buildAllGetGradientObjectiveKernels() {
        int b = 0;
        //for (FormatType formatType : neededFormatTypes) {
        	buildGetGradientObjectiveKernel(true); ++b;
        	buildGetGradientObjectiveKernel(false); ++b;
        //}
    }

    void buildAllSyncCVGetGradientObjectiveKernels() {
        int b = 0;
        buildSyncCVGetGradientObjectiveKernel(); ++b;
    }

    void buildAllGetLogLikelihoodKernels() {
        int b = 0;
    	buildGetLogLikelihoodKernel(true); ++b;
    	buildGetLogLikelihoodKernel(false); ++b;
    }

    void buildAllDoItAllKernels(const std::vector<FormatType>& neededFormatTypes) {
        int b = 0;
        for (FormatType formatType : neededFormatTypes) {
            buildDoItAllKernel(formatType, 0);
            buildDoItAllKernel(formatType, 1);
            buildDoItAllKernel(formatType, 2);

            buildMMFindDeltaKernel(formatType, 0);
            buildMMFindDeltaKernel(formatType, 1);
            buildMMFindDeltaKernel(formatType, 2);
        }
    }

    std::string getFormatTypeExtension(FormatType formatType) {
        switch (formatType) {
        case DENSE:
            return "Den";
        case SPARSE:
            return "Spa";
        case INDICATOR:
            return "Ind";
        case INTERCEPT:
            return "Icp";
        default: return "";
        }
    }

    SourceCode writeCodeForEmptyKernel();

    SourceCode writeCodeForGradientHessianKernel(FormatType formatType, bool useWeights, bool isNvidia);

    SourceCode writeCodeForUpdateXBetaKernel(FormatType formatType);

    SourceCode writeCodeForSyncUpdateXBetaKernel(FormatType formatType);

    SourceCode writeCodeForSync1UpdateXBetaKernel(FormatType formatType);

    SourceCode writeCodeForUpdateAllXBetaKernel(FormatType formatType, bool isNvidia);

    SourceCode writeCodeForMMGradientHessianKernel(FormatType formatType, bool useWeights, bool isNvidia);

    SourceCode writeCodeForSyncCVMMGradientHessianKernel(FormatType formatType, bool isNvidia);

    SourceCode writeCodeForAllGradientHessianKernel(FormatType formatType, bool useWeights, bool isNvidia);

    SourceCode writeCodeForSyncCVGradientHessianKernel(FormatType formatType, bool isNvidia);

    SourceCode writeCodeForSyncCV1GradientHessianKernel(FormatType formatType, bool isNvidia);

    SourceCode writeCodeForGetGradientObjective(bool useWeights, bool isNvidia);

    SourceCode writeCodeForGetGradientObjectiveSync(bool isNvidia);

    SourceCode writeCodeForComputeRemainingStatisticsKernel();

    SourceCode writeCodeForSyncComputeRemainingStatisticsKernel();

    SourceCode writeCodeForGradientHessianKernelExactCLR(FormatType formatType, bool useWeights, bool isNvidia);

    SourceCode writeCodeForGetLogLikelihood(bool useWeights, bool isNvidia);

    SourceCode writeCodeForMMUpdateXBetaKernel(bool isNvidia);

    SourceCode writeCodeForReduceCVBuffer();

    SourceCode writeCodeForProcessDeltaKernel(int priorType);

    SourceCode writeCodeForDoItAllKernel(FormatType formatType, int priorType);

    SourceCode writeCodeForMMFindDeltaKernel(FormatType formatType, int priorType);

    void buildDoItAllKernel(FormatType formatType, int priorType) {
        std::stringstream options;

        if (sizeof(real) == 8) {
#ifdef USE_VECTOR
        options << "-DREAL=double -DTMP_REAL=double2 ";
#else
        options << "-DREAL=double -DTMP_REAL=double ";
#endif // USE_VECTOR
        } else {
#ifdef USE_VECTOR
            options << "-DREAL=float -DTMP_REAL=float2 ";
#else
            options << "-DREAL=float -DTMP_REAL=float ";
#endif // USE_VECTOR
        }
        options << " -cl-mad-enable";

    	auto source = writeCodeForDoItAllKernel(formatType, priorType);
    	auto program = compute::program::build_with_source(source.body, ctx, options.str());
    	auto kernel = compute::kernel(program, source.name);

    	kernelDoItAll[formatType*3+priorType] = std::move(kernel);
    }

    void buildMMFindDeltaKernel(FormatType formatType, int priorType) {
            std::stringstream options;

            if (sizeof(real) == 8) {
    #ifdef USE_VECTOR
            options << "-DREAL=double -DTMP_REAL=double2 ";
    #else
            options << "-DREAL=double -DTMP_REAL=double ";
    #endif // USE_VECTOR
            } else {
    #ifdef USE_VECTOR
                options << "-DREAL=float -DTMP_REAL=float2 ";
    #else
                options << "-DREAL=float -DTMP_REAL=float ";
    #endif // USE_VECTOR
            }
            options << " -cl-mad-enable";

        	auto source = writeCodeForMMFindDeltaKernel(formatType, priorType);
        	auto program = compute::program::build_with_source(source.body, ctx, options.str());
        	auto kernel = compute::kernel(program, source.name);

        	kernelMMFindDelta[formatType*3+priorType] = std::move(kernel);
        }

    void buildProcessDeltaKernel(int priorType) {
        std::stringstream options;

        if (sizeof(real) == 8) {
#ifdef USE_VECTOR
        options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << tpb;
#else
        options << "-DREAL=double -DTMP_REAL=double -DTPB=" << tpb;
#endif // USE_VECTOR
        } else {
#ifdef USE_VECTOR
            options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << tpb;
#else
            options << "-DREAL=float -DTMP_REAL=float -DTPB=" << tpb;
#endif // USE_VECTOR
        }
        options << " -cl-mad-enable";

    	auto source = writeCodeForProcessDeltaKernel(priorType);
    	auto program = compute::program::build_with_source(source.body, ctx, options.str());
    	auto kernel = compute::kernel(program, source.name);

    	kernelProcessDeltaBuffer[priorType] = std::move(kernel);
    }

    void buildGradientHessianKernel(FormatType formatType, bool useWeights) {

        std::stringstream options;

        if (sizeof(real) == 8) {
#ifdef USE_VECTOR
        options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << tpb;
#else
        options << "-DREAL=double -DTMP_REAL=double -DTPB=" << tpb;
#endif // USE_VECTOR
        } else {
#ifdef USE_VECTOR
            options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << tpb;
#else
            options << "-DREAL=float -DTMP_REAL=float -DTPB=" << tpb;
#endif // USE_VECTOR
        }
        options << " -cl-mad-enable";

//         compute::vector<compute::double2_> buf(10, ctx);
//
//         compute::double2_ sum = compute::double2_{0.0, 0.0};
//         compute::reduce(buf.begin(), buf.end(), &sum, queue);
//
//         std::cerr << sum << std::endl;
//
//         auto cache = compute::program_cache::get_global_cache(ctx);
//         auto list = cache->get_keys();
//         std::cerr << "list size = " << list.size() << std::endl;
//         for (auto a : list) {
//             std::cerr << a.first << ":" << a.second << std::endl;
//             auto p = cache->get(a.first, a.second);
//             if (p) {
//                 std::cerr << p->source() << std::endl;
//             }
//         }
//
//         Rcpp::stop("out");

        auto isNvidia = compute::detail::is_nvidia_device(queue.get_device());
        isNvidia = false;
        //std::cout << "formatType: " << formatType << " isNvidia: " << isNvidia << '\n';

//         std::cerr << queue.get_device().name() << " " << queue.get_device().vendor() << std::endl;
//         std::cerr << "isNvidia = " << isNvidia << std::endl;
//         Rcpp::stop("out");

        if (BaseModel::exactCLR) {
        	// CCD Kernel
        	auto source = writeCodeForGradientHessianKernelExactCLR(formatType, useWeights, isNvidia);
        	auto program = compute::program::build_with_source(source.body, ctx, options.str());
        	auto kernel = compute::kernel(program, source.name);

        	if (useWeights) {
        		kernelGradientHessianWeighted[formatType] = std::move(kernel);
        		//kernelGradientHessianMMWeighted[formatType] = std::move(kernelMM);
        		//kernelGradientHessianAllWeighted[formatType] = std::move(kernelAll);
        		//kernelGradientHessianSyncWeighted[formatType] = std::move(kernelSync);
        	} else {
        		kernelGradientHessianNoWeight[formatType] = std::move(kernel);
        		//kernelGradientHessianMMNoWeight[formatType] = std::move(kernelMM);
        		//kernelGradientHessianAllNoWeight[formatType] = std::move(kernelAll);
        		//kernelGradientHessianSyncNoWeight[formatType] = std::move(kernelSync);
        	}
        } else {
        	// CCD Kernel
        	auto source = writeCodeForGradientHessianKernel(formatType, useWeights, isNvidia);
        	auto program = compute::program::build_with_source(source.body, ctx, options.str());
        	auto kernel = compute::kernel(program, source.name);
        	// Rcpp::stop("cGH");

        	// MM Kernel
        	//if (algorithmType == AlgorithmType::MM) {
        	source = writeCodeForMMGradientHessianKernel(formatType, useWeights, isNvidia);
        	program = compute::program::build_with_source(source.body, ctx, options.str());
        	auto kernelMM = compute::kernel(program, source.name);

        	if (useWeights) {
        		kernelGradientHessianMMWeighted[formatType] = std::move(kernelMM);
        	} else {
        		kernelGradientHessianMMNoWeight[formatType] = std::move(kernelMM);
        	}
        	//}

        	source = writeCodeForAllGradientHessianKernel(formatType, useWeights, isNvidia);
        	program = compute::program::build_with_source(source.body, ctx, options.str());
        	auto kernelAll = compute::kernel(program, source.name);
        	if (useWeights) {
        		kernelGradientHessianAllWeighted[formatType] = std::move(kernelAll);
        	} else {
        		kernelGradientHessianAllNoWeight[formatType] = std::move(kernelAll);
        	}


        	if (useWeights) {
        		kernelGradientHessianWeighted[formatType] = std::move(kernel);
        	} else {
        		kernelGradientHessianNoWeight[formatType] = std::move(kernel);
        	}
        }
    }

    void buildSyncCVGradientHessianKernel(FormatType formatType) {

        std::stringstream options;

        int thisTPB = 32;
        if (syncCVFolds > 32) thisTPB = 64;
        if (syncCVFolds > 64) thisTPB = 128;

        if (sizeof(real) == 8) {
#ifdef USE_VECTOR
            options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << thisTPB;
#else
            options << "-DREAL=double -DTMP_REAL=double -DTPB=" << thisTPB;
#endif // USE_VECTOR
        } else {
#ifdef USE_VECTOR
            options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << thisTPB;
#else
            options << "-DREAL=float -DTMP_REAL=float -DTPB=" << thisTPB;
#endif // USE_VECTOR
        }
        options << " -cl-mad-enable";

        auto isNvidia = compute::detail::is_nvidia_device(queue.get_device());
        isNvidia = false;

        // CCD Kernel
        // Rcpp::stop("cGH");
        auto source = writeCodeForSyncCVGradientHessianKernel(formatType, isNvidia);
        auto program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernelSync = compute::kernel(program, source.name);
        kernelGradientHessianSync[formatType] = std::move(kernelSync);

        source = writeCodeForSyncCV1GradientHessianKernel(formatType, isNvidia);
        program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernelSync1 = compute::kernel(program, source.name);
        kernelGradientHessianSync1[formatType] = std::move(kernelSync1);

        // MM Kernel
        //if (algorithmType == AlgorithmType::MM) {

        source = writeCodeForSyncCVMMGradientHessianKernel(formatType, isNvidia);
        program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernelMMSync = compute::kernel(program, source.name);
        kernelGradientHessianMMSync[formatType] = std::move(kernelMMSync);
    }

    void buildUpdateXBetaKernel(FormatType formatType) {
        std::stringstream options;

        options << "-DREAL=" << (sizeof(real) == 8 ? "double" : "float");

    	options << " -cl-mad-enable";

        auto source = writeCodeForUpdateXBetaKernel(formatType);

        auto program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernel = compute::kernel(program, source.name);

        // Rcpp::stop("uXB");

        // Run-time constant arguments.
        kernelUpdateXBeta[formatType] = std::move(kernel);
    }

    void buildSyncCVUpdateXBetaKernel(FormatType formatType) {
        std::stringstream options;

        options << "-DREAL=" << (sizeof(real) == 8 ? "double" : "float");

        options << " -cl-mad-enable";

        // Rcpp::stop("uXB");

        // Run-time constant arguments.

        auto source = writeCodeForSyncUpdateXBetaKernel(formatType);
        std::cout << source.body;
        auto program = compute::program::build_with_source(source.body, ctx, options.str());
        std::cout << "program built\n";
        auto kernelSync = compute::kernel(program, source.name);
        kernelUpdateXBetaSync[formatType] = std::move(kernelSync);

        source = writeCodeForSync1UpdateXBetaKernel(formatType);
        program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernelSync1 = compute::kernel(program, source.name);
        kernelUpdateXBetaSync1[formatType] = std::move(kernelSync1);
    }

    void buildEmptyKernel() {
        std::stringstream options;

        options << "-DREAL=" << (sizeof(real) == 8 ? "double" : "float");

        options << " -cl-mad-enable";

        auto source = writeCodeForEmptyKernel();
        auto program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernelSync = compute::kernel(program, source.name);
        kernelEmpty = std::move(kernelSync);
    }

    void buildUpdateXBetaMMKernel() {
    	std::stringstream options;

    	if (sizeof(real) == 8) {
#ifdef USE_VECTOR
    		options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << 32;
#else
    		options << "-DREAL=double -DTMP_REAL=double -DTPB=" << 32;
#endif // USE_VECTOR
    	} else {
#ifdef USE_VECTOR
    		options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << 32;
#else
    		options << "-DREAL=float -DTMP_REAL=float -DTPB=" << 32;
#endif // USE_VECTOR
    	}
    	options << " -cl-mad-enable";

    	auto isNvidia = compute::detail::is_nvidia_device(queue.get_device());
    	isNvidia = false;
    	auto source = writeCodeForMMUpdateXBetaKernel(isNvidia);
    	auto program = compute::program::build_with_source(source.body, ctx, options.str());
    	auto kernelMM = compute::kernel(program, source.name);
    	kernelUpdateXBetaMM = std::move(kernelMM);
    }


    void buildComputeRemainingStatisticsKernel() {
    	std::stringstream options;
    	options << "-DREAL=" << (sizeof(real) == 8 ? "double" : "float");

    	options << " -cl-mad-enable";

        auto source = writeCodeForComputeRemainingStatisticsKernel();
        auto program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernel = compute::kernel(program, source.name);

        kernelComputeRemainingStatistics = std::move(kernel);
    }

    void buildSyncCVComputeRemainingStatisticsKernel() {
        std::stringstream options;
        options << "-DREAL=" << (sizeof(real) == 8 ? "double" : "float");

        options << " -cl-mad-enable";
        auto source = writeCodeForSyncComputeRemainingStatisticsKernel();
        auto program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernelSync = compute::kernel(program, source.name);

        kernelComputeRemainingStatisticsSync = std::move(kernelSync);
    }

    void buildUpdateAllXBetaKernel(FormatType formatType) {
      	std::stringstream options;

        if (sizeof(real) == 8) {
#ifdef USE_VECTOR
        options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << detail::constant::updateAllXBetaBlockSize;
#else
        options << "-DREAL=double -DTMP_REAL=double -DTPB=" << detail::constant::updateAllXBetaBlockSize;
#endif // USE_VECTOR
        } else {
#ifdef USE_VECTOR
            options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << detail::constant::updateAllXBetaBlockSize;
#else
            options << "-DREAL=float -DTMP_REAL=float -DTPB=" << detail::constant::updateAllXBetaBlockSize;
#endif // USE_VECTOR
        }

    	options << " -cl-mad-enable";

    	//options << "-DREAL=" << (sizeof(real) == 8 ? "double" : "float");

        auto isNvidia = compute::detail::is_nvidia_device(queue.get_device());
        isNvidia = false;

      	auto source = writeCodeForUpdateAllXBetaKernel(formatType, isNvidia);
      	auto program = compute::program::build_with_source(source.body, ctx, options.str());
      	auto kernel = compute::kernel(program, source.name);


      	kernelUpdateAllXBeta[formatType] = std::move(kernel);
    }

    void buildGetGradientObjectiveKernel(bool useWeights) {
    	std::stringstream options;
        if (sizeof(real) == 8) {
#ifdef USE_VECTOR
        options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << tpb;
#else
        options << "-DREAL=double -DTMP_REAL=double -DTPB=" << tpb;
#endif // USE_VECTOR
        } else {
#ifdef USE_VECTOR
            options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << tpb;
#else
            options << "-DREAL=float -DTMP_REAL=float -DTPB=" << tpb;
#endif // USE_VECTOR
        }
        options << " -cl-mad-enable";

         auto isNvidia = compute::detail::is_nvidia_device(queue.get_device());
         isNvidia = false;

         auto source = writeCodeForGetGradientObjective(useWeights, isNvidia);
         auto program = compute::program::build_with_source(source.body, ctx, options.str());
         auto kernel = compute::kernel(program, source.name);

         int dK = K;

         // Run-time constant arguments.

         if (useWeights) {
             kernelGetGradientObjectiveWeighted = std::move(kernel);
         } else {
        	 kernelGetGradientObjectiveNoWeight = std::move(kernel);
         }
    }

    void buildSyncCVGetGradientObjectiveKernel() {
        std::stringstream options;
        if (sizeof(real) == 8) {
#ifdef USE_VECTOR
            options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << tpb;
#else
            options << "-DREAL=double -DTMP_REAL=double -DTPB=" << tpb;
#endif // USE_VECTOR
        } else {
#ifdef USE_VECTOR
            options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << tpb;
#else
            options << "-DREAL=float -DTMP_REAL=float -DTPB=" << tpb;
#endif // USE_VECTOR
        }
        options << " -cl-mad-enable";

        auto isNvidia = compute::detail::is_nvidia_device(queue.get_device());
        isNvidia = false;

        // Run-time constant arguments.
        auto source = writeCodeForGetGradientObjectiveSync(isNvidia);
        auto program = compute::program::build_with_source(source.body, ctx, options.str());
        auto kernelSync = compute::kernel(program, source.name);
        kernelGetGradientObjectiveSync = std::move(kernelSync);
    }

    void buildGetLogLikelihoodKernel(bool useWeights) {
    	std::stringstream options;
        if (sizeof(real) == 8) {
#ifdef USE_VECTOR
        options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << tpb;
#else
        options << "-DREAL=double -DTMP_REAL=double -DTPB=" << tpb;
#endif // USE_VECTOR
        } else {
#ifdef USE_VECTOR
            options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << tpb;
#else
            options << "-DREAL=float -DTMP_REAL=float -DTPB=" << tpb;
#endif // USE_VECTOR
        }
        options << " -cl-mad-enable";

         auto isNvidia = compute::detail::is_nvidia_device(queue.get_device());
         isNvidia = false;

         auto source = writeCodeForGetLogLikelihood(useWeights, isNvidia);
         auto program = compute::program::build_with_source(source.body, ctx, options.str());
         auto kernel = compute::kernel(program, source.name);

         int dK = K;
         int dN = N;
         // Run-time constant arguments.

         if (useWeights) {
             kernelGetLogLikelihoodWeighted = std::move(kernel);
         } else {
        	 kernelGetLogLikelihoodNoWeight = std::move(kernel);
         }
    }

    void buildReduceCVBufferKernel() {
       	std::stringstream options;
           if (sizeof(real) == 8) {
   #ifdef USE_VECTOR
           options << "-DREAL=double -DTMP_REAL=double2 -DTPB=" << tpb;
   #else
           options << "-DREAL=double -DTMP_REAL=double -DTPB=" << tpb;
   #endif // USE_VECTOR
           } else {
   #ifdef USE_VECTOR
               options << "-DREAL=float -DTMP_REAL=float2 -DTPB=" << tpb;
   #else
               options << "-DREAL=float -DTMP_REAL=float -DTPB=" << tpb;
   #endif // USE_VECTOR
           }
           options << " -cl-mad-enable";

            auto source = writeCodeForReduceCVBuffer();
            auto program = compute::program::build_with_source(source.body, ctx, options.str());
            auto kernel = compute::kernel(program, source.name);

            kernelReduceCVBuffer = std::move(kernel);
       }

    void printKernel(compute::kernel& kernel, std::ostream& stream) {

        auto program = kernel.get_program();
        auto buildOptions = program.get_build_info<std::string>(CL_PROGRAM_BUILD_OPTIONS, device);

        stream // TODO Change to R
            << "Options: " << buildOptions << std::endl
            << program.source()
            << std::endl;
    }

    void buildAllKernels(const std::vector<FormatType>& neededFormatTypes) {
        buildAllGradientHessianKernels(neededFormatTypes);
        std::cout << "built gradhessian kernels \n";
        buildAllUpdateXBetaKernels(neededFormatTypes);
        std::cout << "built updateXBeta kernels \n";
        buildAllGetGradientObjectiveKernels();
        std::cout << "built getGradObjective kernels \n";
        buildAllGetLogLikelihoodKernels();
        std::cout << "built getLogLikelihood kernels \n";
        buildAllComputeRemainingStatisticsKernels();
        std::cout << "built computeRemainingStatistics kernels \n";
        buildEmptyKernel();
        std::cout << "built empty kernel\n";
        //buildReduceCVBufferKernel();
        //std::cout << "built reduceCVBuffer kenel\n";
        //buildAllProcessDeltaKernels();
        //std::cout << "built ProcessDelta kenels \n";
        //buildAllDoItAllKernels(neededFormatTypes);
        //std::cout << "built doItAll kernels\n";
    }

    void buildAllSyncCVKernels(const std::vector<FormatType>& neededFormatTypes) {
        buildAllSyncCVGradientHessianKernels(neededFormatTypes);
        std::cout << "built syncCV gradhessian kernels \n";
        buildAllSyncCVUpdateXBetaKernels(neededFormatTypes);
        std::cout << "built syncCV updateXBeta kernels \n";
        buildAllSyncCVGetGradientObjectiveKernels();
        std::cout << "built syncCV getGradObjective kernels \n";
        //buildAllGetLogLikelihoodKernels();
        //std::cout << "built getLogLikelihood kernels \n";
        buildAllSyncCVComputeRemainingStatisticsKernels();
        std::cout << "built computeRemainingStatistics kernels \n";
        buildReduceCVBufferKernel();
        std::cout << "built reduceCVBuffer kenel\n";
        buildAllProcessDeltaKernels();
        std::cout << "built ProcessDelta kenels \n";
        buildAllDoItAllKernels(neededFormatTypes);
        std::cout << "built doItAll kernels\n";
    }

    void printAllKernels(std::ostream& stream) {
    	for (auto& entry : kernelGradientHessianWeighted) {
    		printKernel(entry.second, stream);
    	}

    	for (auto& entry : kernelGradientHessianNoWeight) {
    		printKernel(entry.second, stream);
    	}
    	for (auto& entry : kernelGradientHessianMMWeighted) {
    		printKernel(entry.second, stream);
    	}

    	for (auto& entry : kernelGradientHessianMMNoWeight) {
    		printKernel(entry.second, stream);
    	}

    	for (auto& entry : kernelGradientHessianAllWeighted) {
    		printKernel(entry.second, stream);
    	}

    	for (auto& entry : kernelGradientHessianAllNoWeight) {
    		printKernel(entry.second, stream);
    	}

        for (auto& entry : kernelUpdateXBeta) {
            printKernel(entry.second, stream);
        }

        for (auto& entry : kernelUpdateAllXBeta) {
            printKernel(entry.second, stream);
        }

        printKernel(kernelGetGradientObjectiveWeighted, stream);
        printKernel(kernelGetGradientObjectiveNoWeight, stream);

        printKernel(kernelComputeRemainingStatistics, stream);
    }

    void printAllSyncCVKernels(std::ostream& stream) {

    	for (auto& entry : kernelGradientHessianSync) {
    		printKernel(entry.second, stream);
    	}

    	for (auto& entry : kernelGradientHessianMMSync) {
    		printKernel(entry.second, stream);
    	}

    	printKernel(kernelReduceCVBuffer, stream);

    	for (auto& entry : kernelUpdateXBetaSync) {
    		printKernel(entry.second, stream);
    	}

    	printKernel(kernelUpdateXBetaMM, stream);

    	for (auto& entry : kernelDoItAll) {
    		printKernel(entry.second, stream);
    	}

    	for (auto& entry : kernelMMFindDelta) {
    		printKernel(entry.second, stream);
    	}

    	for (auto& entry : kernelProcessDeltaBuffer) {
    		printKernel(entry.second, stream);
    	}

    	printKernel(kernelGetGradientObjectiveSync, stream);

    	printKernel(kernelComputeRemainingStatisticsSync, stream);
    }



    // boost::compute objects
    const compute::device device;
    const compute::context ctx;
    compute::command_queue queue;
    compute::program program;

    std::map<FormatType, compute::kernel> kernelGradientHessianWeighted;
    std::map<FormatType, compute::kernel> kernelGradientHessianNoWeight;
    std::map<FormatType, compute::kernel> kernelUpdateXBeta;
    std::map<FormatType, compute::kernel> kernelUpdateXBetaSync;
    std::map<FormatType, compute::kernel> kernelUpdateXBetaSync1;
    std::map<FormatType, compute::kernel> kernelGradientHessianMMWeighted;
    std::map<FormatType, compute::kernel> kernelGradientHessianMMNoWeight;
    std::map<FormatType, compute::kernel> kernelGradientHessianMMSync;
    std::map<FormatType, compute::kernel> kernelGradientHessianAllWeighted;
    std::map<FormatType, compute::kernel> kernelGradientHessianAllNoWeight;
    std::map<FormatType, compute::kernel> kernelGradientHessianSync;
    std::map<FormatType, compute::kernel> kernelGradientHessianSync1;

    std::map<int, compute::kernel> kernelDoItAll;
    std::map<int, compute::kernel> kernelMMFindDelta;
    std::map<int, compute::kernel> kernelProcessDeltaBuffer;

    compute::kernel kernelEmpty;
    compute::kernel kernelReduceCVBuffer;
    compute::kernel kernelUpdateXBetaMM;
    compute::kernel kernelGetGradientObjectiveWeighted;
    compute::kernel kernelGetGradientObjectiveNoWeight;
    compute::kernel kernelGetGradientObjectiveSync;
    compute::kernel kernelComputeRemainingStatistics;
    compute::kernel kernelComputeRemainingStatisticsSync;
    compute::kernel kernelGetLogLikelihoodWeighted;
    compute::kernel kernelGetLogLikelihoodNoWeight;
    std::map<FormatType, compute::kernel> kernelUpdateAllXBeta;
    std::map<FormatType, std::vector<int>> indicesFormats;
    std::vector<FormatType> formatList;

    // vectors of columns
    // std::vector<GpuColumn<real> > columns;
    AllGpuColumns<real> dColumns;
    AllGpuColumns<real> dColumnsXt;

    std::vector<real> hBuffer0;
    std::vector<real> hBuffer;
    std::vector<real> hBuffer1;
    std::vector<real> xMatrix;
    std::vector<real> expXMatrix;
	std::vector<real> hFirstRow;
	std::vector<real> hOverflow;

    // Internal storage
    compute::vector<real> dY;
    compute::vector<real> dBeta;
    compute::vector<real> dXBeta;
    compute::vector<real> dExpXBeta;
    compute::vector<real> dDenominator;
    compute::vector<real> dAccDenominator;
    compute::vector<real> dNorm;
    compute::vector<real> dOffs;
    compute::vector<int>  dFixBeta;
    compute::vector<real> dAllDelta;

    // for exactCLR
    std::vector<int> subjects;
    int totalCases;
    int maxN;
    int maxCases;
    compute::vector<real>  dRealVector1;
    compute::vector<real>  dRealVector2;
    compute::vector<int>  dIntVector1;
    compute::vector<int>  dIntVector2;
    compute::vector<int>  dIntVector3;
    compute::vector<int>  dIntVector4;
    compute::vector<real> dXMatrix;
    compute::vector<real> dExpXMatrix;
    bool initialized = false;
    compute::vector<real> dOverflow0;
    compute::vector<real> dOverflow1;
    compute::vector<int> dNtoK;
    compute::vector<real> dFirstRow;
    compute::vector<int> dAllZero;

#ifdef USE_VECTOR
    compute::vector<compute::double2_> dBuffer;
#else
    compute::vector<real> dBuffer;
    compute::vector<real> dBuffer1;
#endif // USE_VECTOR
    compute::vector<real> dKWeight;	//TODO make these weighttype
    compute::vector<real> dNWeight; //TODO make these weighttype
    compute::vector<int> dId;

    bool dXBetaKnown;
    bool hXBetaKnown;

    // syhcCV
    int cvBlockSize;
    int cvIndexStride;
    bool pad;
    compute::vector<real> dNWeightVector;
    compute::vector<real> dKWeightVector;
    compute::vector<real> dAccDenomPidVector;
    compute::vector<real> dAccNumerPidVector;
    compute::vector<real> dAccNumerPid2Vector;
    compute::vector<int> dAccResetVector;
    compute::vector<int> dPidVector;
    compute::vector<int> dPidInternalVector;
    compute::vector<real> dXBetaVector;
    compute::vector<real> dOffsExpXBetaVector;
    compute::vector<real> dDenomPidVector;
    compute::vector<real> dNumerPidVector;
    compute::vector<real> dNumerPid2Vector;
    compute::vector<real> dXjYVector;
    compute::vector<real> dXjXVector;
    //compute::vector<real> dLogLikelihoodFixedTermVector;
    //compute::vector<IndexVectorPtr> dSparseIndicesVector;
    compute::vector<real> dNormVector;
    compute::vector<real> dDeltaVector;
    compute::vector<real> dBoundVector;
    compute::vector<real> dPriorParams;
    compute::vector<real> dBetaVector;
    compute::vector<int> dDoneVector;

    std::vector<real> priorTypes;
    std::vector<int> indexListWithPrior[12];
};

/*
static std::string timesX(const std::string& arg, const FormatType formatType) {
    return (formatType == INDICATOR || formatType == INTERCEPT) ?
        arg : arg + " * x";
}

static std::string weight(const std::string& arg, bool useWeights) {
    return useWeights ? "w * " + arg : arg;
}
*/

static std::string timesX(const std::string& arg, const FormatType formatType) {
    return (formatType == INDICATOR || formatType == INTERCEPT) ?
        arg : arg + " * x";
}

static std::string weight(const std::string& arg, bool useWeights) {
    return useWeights ? "w * " + arg : arg;
}

static std::string weightK(const std::string& arg, bool useWeights) {
    return useWeights ? "wK * " + arg : arg;
}

static std::string weightN(const std::string& arg, bool useWeights) {
    return useWeights ? "wN * " + arg : arg;
}

struct GroupedDataG {
public:
	std::string getGroupG() {
		std::string code = "task";
        return(code);
	}
};

struct GroupedWithTiesDataG : GroupedDataG {
public:
};

struct OrderedDataG {
public:
	std::string getGroupG() {
		std::string code = "task";
        return(code);
	}
};

struct OrderedWithTiesDataG {
public:
	std::string getGroupG() {
		std::string code = "id[task]";
        return(code);
	}
};

struct IndependentDataG {
public:
	std::string getGroupG() {
		std::string code = "task";
        return(code);
	}
};

struct FixedPidG {
};

struct SortedPidG {
};

struct NoFixedLikelihoodTermsG {
};

#define Fraction std::complex

struct GLMProjectionG {
public:
	std::string logLikeNumeratorContribG() {
		std::stringstream code;
		code << "y * xb";
		return(code.str());
	}
};

struct SurvivalG {
public:
};

struct LogisticG {
public:
	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		// assume exists: numer, denom
		std::stringstream code;
        code << "       REAL g = numer / denom;      \n";
        code << "       REAL gradient = " << weight("g", useWeights) << ";\n";
        if (formatType == INDICATOR || formatType == INTERCEPT) {
            code << "       REAL hessian  = " << weight("g * ((REAL)1.0 - g)", useWeights) << ";\n";
        } else {
            code << "       REAL nume2 = " << timesX("numer", formatType) << ";\n" <<
                    "       REAL hessian  = " << weight("(nume2 / denom - g * g)", useWeights) << ";\n";
        }
        return(code.str());
	}

};

struct SelfControlledCaseSeriesG : public GroupedDataG, GLMProjectionG, FixedPidG, SurvivalG {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)0.0";
		return(code);
	}

	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		std::stringstream code;
        code << "       REAL g = numer / denom;      \n";
        code << "       REAL gradient = " << weight("g", useWeights) << ";\n";
        if (formatType == INDICATOR || formatType == INTERCEPT) {
            code << "       REAL hessian  = " << weight("g * ((REAL)1.0 - g)", useWeights) << ";\n";
        } else {
            code << "       REAL nume2 = " << timesX("numer", formatType) << ";\n" <<
                    "       REAL hessian  = " << weight("(nume2 / denom - g * g)", useWeights) << ";\n";
        }
        return(code.str());
	}

    std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "offs * exp(xb)";
        return(code.str());
    }

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "wN * log(denom)";
		return(code.str());
	}

};

struct ConditionalPoissonRegressionG : public GroupedDataG, GLMProjectionG, FixedPidG, SurvivalG {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)0.0";
		return(code);
	}

	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		std::stringstream code;
        code << "       REAL g = numer / denom;      \n";
        code << "       REAL gradient = " << weight("g", useWeights) << ";\n";
        if (formatType == INDICATOR || formatType == INTERCEPT) {
            code << "       REAL hessian  = " << weight("g * ((REAL)1.0 - g)", useWeights) << ";\n";
        } else {
            code << "       REAL nume2 = " << timesX("numer", formatType) << ";\n" <<
                    "       REAL hessian  = " << weight("(nume2 / denom - g * g)", useWeights) << ";\n";
        }
        return(code.str());
	}

    std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "exp(xb)";
        return(code.str());
    }

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "wN * log(denom)";
		return(code.str());
	}

};

struct ConditionalLogisticRegressionG : public GroupedDataG, GLMProjectionG, FixedPidG, SurvivalG {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)0.0";
		return(code);
	}

	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		std::stringstream code;
        code << "       REAL g = numer / denom;      \n";
        code << "       REAL gradient = " << weight("g", useWeights) << ";\n";
        if (formatType == INDICATOR || formatType == INTERCEPT) {
            code << "       REAL hessian  = " << weight("g * ((REAL)1.0 - g)", useWeights) << ";\n";
        } else {
            code << "       REAL nume2 = " << timesX("numer", formatType) << ";\n" <<
                    "       REAL hessian  = " << weight("(nume2 / denom - g * g)", useWeights) << ";\n";
        }
        return(code.str());
	}

    std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "exp(xb)";
        return(code.str());
    }

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "wN * log(denom)";
		return(code.str());
	}

};

struct TiedConditionalLogisticRegressionG : public GroupedWithTiesDataG, GLMProjectionG, FixedPidG, SurvivalG {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)0.0";
		return(code);
	}

    std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "exp(xb)";
        return(code.str());
    }

	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		return("");
	}

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "wN * log(denom)";
		return(code.str());
	}

};

struct LogisticRegressionG : public IndependentDataG, GLMProjectionG, LogisticG, FixedPidG,
	NoFixedLikelihoodTermsG {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)1.0";
		return(code);
	}

    std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "exp(xb)";
        return(code.str());
    }

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "log(denom)";
		return(code.str());
	}

};

struct CoxProportionalHazardsG : public OrderedDataG, GLMProjectionG, SortedPidG, NoFixedLikelihoodTermsG, SurvivalG {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)0.0";
		return(code);
	}

	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		std::stringstream code;
        code << "       REAL g = numer / denom;      \n";
        code << "       REAL gradient = " << weight("g", useWeights) << ";\n";
        if (formatType == INDICATOR || formatType == INTERCEPT) {
            code << "       REAL hessian  = " << weight("g * ((REAL)1.0 - g)", useWeights) << ";\n";
        } else {
            code << "       REAL nume2 = " << timesX("numer", formatType) << ";\n" <<
                    "       REAL hessian  = " << weight("(nume2 / denom - g * g)", useWeights) << ";\n";
        }
        return(code.str());
	}

    std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "exp(xb)";
        return(code.str());
    }

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "wN * log(denom)";
		return(code.str());
	}

};

struct StratifiedCoxProportionalHazardsG : public CoxProportionalHazardsG {
public:
};

struct BreslowTiedCoxProportionalHazardsG : public OrderedWithTiesDataG, GLMProjectionG, SortedPidG, NoFixedLikelihoodTermsG, SurvivalG {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)0.0";
		return(code);
	}

    std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "exp(xb)";
        return(code.str());
    }

	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		std::stringstream code;
        code << "       REAL g = numer / denom;      \n";
        code << "       REAL gradient = " << weight("g", useWeights) << ";\n";
        if (formatType == INDICATOR || formatType == INTERCEPT) {
            code << "       REAL hessian  = " << weight("g * ((REAL)1.0 - g)", useWeights) << ";\n";
        } else {
            code << "       REAL nume2 = " << timesX("numer", formatType) << ";\n" <<
                    "       REAL hessian  = " << weight("(nume2 / denom - g * g)", useWeights) << ";\n";
        }
        return(code.str());
	}

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "wN * log(denom)";
		return(code.str());
	}
};

struct LeastSquaresG : public IndependentDataG, FixedPidG, NoFixedLikelihoodTermsG  {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)0.0";
		return(code);
	}

	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		return("");
	}

	std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "(REAL)0.0";
        return(code.str());
    }

	std::string logLikeNumeratorContribG() {
		std::stringstream code;
		code << "(y-xb)*(y-xb)";
		return(code.str());
	}

	real logLikeDenominatorContrib(int ni, real denom) {
		return std::log(denom);
	}

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "log(denom)";
		return(code.str());
	}

};

struct PoissonRegressionG : public IndependentDataG, GLMProjectionG, FixedPidG {
public:
	std::string getDenomNullValueG () {
		std::string code = "(REAL)0.0";
		return(code);
	}

	std::string incrementGradientAndHessianG(FormatType formatType, bool useWeights) {
		std::stringstream code;
        code << "       REAL gradient = " << weight("numer", useWeights) << ";\n";
        if (formatType == INDICATOR || formatType == INTERCEPT) {
            code << "       REAL hessian  = gradient;\n";
        } else {
            code << "       REAL nume2 = " << timesX("numer", formatType) << ";\n" <<
                    "       REAL hessian  = " << weight("nume2", useWeights) << ";\n";
        }
        return(code.str());
	}

    std::string getOffsExpXBetaG() {
		std::stringstream code;
		code << "exp(xb)";
        return(code.str());
    }

	std::string logLikeDenominatorContribG() {
		std::stringstream code;
		code << "denom";
		return(code.str());
	}

};


} // namespace bsccs


#include "Kernels.hpp"

#endif /* GPUMODELSPECIFICS_HPP_ */