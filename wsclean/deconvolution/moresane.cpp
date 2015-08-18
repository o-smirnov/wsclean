#include "moresane.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <unistd.h>

#include "../fitsreader.h"
#include "../fitswriter.h"
#include "../fftconvolver.h"

void MoreSane::ExecuteMajorIteration(double* dataImage, double* modelImage, const double* psfImage, size_t width, size_t height, bool& reachedMajorThreshold)
{
	if(_iterationNumber!=0)
	{
		std::cout << "Convolving model with psf...\n";
		ImageBufferAllocator::Ptr preparedPsf;
		_allocator->Allocate(width*height, preparedPsf);
		FFTConvolver::PrepareKernel(preparedPsf.data(), psfImage, width, height);
		FFTConvolver::ConvolveSameSize(modelImage, preparedPsf.data(), width, height);
		std::cout << "Adding model back to residual...\n";
		for(size_t i=0; i!=width*height; ++i)
			dataImage[i] += modelImage[i];
	}
	std::ostringstream outputStr;
	outputStr << _prefixName << "-tmp-moresaneoutput" << _iterationNumber;
	const std::string
		dirtyName(_prefixName + "-tmp-moresaneinput-dirty.fits"),
		psfName(_prefixName + "-tmp-moresaneinput-psf.fits"),
		maskName(_prefixName + "-tmp-moresaneinput-mask.fits"),
		outputName(outputStr.str());
	FitsWriter writer;
	writer.SetImageDimensions(width, height);
	if(this->_cleanMask != 0)
		writer.WriteMask(maskName, _cleanMask);
	writer.Write(dirtyName, dataImage);
	writer.Write(psfName, psfImage);
	
	std::ostringstream commandLine;
	commandLine
		<< "time python \"" << _moresaneLocation << "\" ";
	if(!_allowNegativeComponents)
		commandLine << "-ep ";
	if(this->_cleanMask != 0)
		commandLine << "-m \"" << maskName + "\" ";
	if(!_moresaneArguments.empty())
		commandLine << _moresaneArguments<< ' ';
	commandLine << "\"" << dirtyName << "\" \"" << psfName << "\" \"" <<  outputName << '\"';
	
	if(!_moresaneSigmaLevels.empty()) {
		commandLine << " -sl " << _moresaneSigmaLevels[std::max(_iterationNumber,_moresaneSigmaLevels.size()-1)] << " ";
	}
	
	std::cout << "Running: " << commandLine.str() << std::endl;
	int pid = vfork();
	switch (pid) {
		case -1: // Error
			throw std::runtime_error("Could not vfork() new process for executing MoreSane");
		case 0: // Child
			execl("/bin/sh", "sh", "-c", commandLine.str().c_str(), NULL);
			_exit(127);
	}
	// Wait for process to terminate
	int pStatus;
	do {
		int pidReturn;
		do {
			pidReturn = waitpid(pid, &pStatus, 0);
		} while (pidReturn == -1 && errno == EINTR);
	} while(!WIFEXITED(pStatus) && !WIFSIGNALED(pStatus));
	if(WIFEXITED(pStatus))
	{
		// all good
		// const int exitStatus = WEXITSTATUS(pStatus);
	} else {
		throw std::runtime_error("MoreSane returned an error");
	}
	
	FitsReader modelReader(outputName+"_model.fits");
	modelReader.Read(modelImage);
	FitsReader residualReader(outputName+"_residual.fits");
	residualReader.Read(dataImage);
	
	unlink(dirtyName.c_str());
	unlink(psfName.c_str());
	unlink(maskName.c_str());
	unlink((outputName+"_model.fits").c_str());
	unlink((outputName+"_residual.fits").c_str());
	
	++_iterationNumber;
	
	reachedMajorThreshold = _iterationNumber<_maxIter;
}
