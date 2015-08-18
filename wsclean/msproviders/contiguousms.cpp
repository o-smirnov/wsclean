#include "contiguousms.h"
#include <casacore/measures/Measures/MEpoch.h>
#include <casacore/measures/TableMeasures/ScalarMeasColumn.h>

ContiguousMS::ContiguousMS(const string& msPath, const std::string& dataColumnName, MSSelection selection, PolarizationEnum polOut, bool includeModel) :
	_timestep(0),
	_time(0.0),
	_dataDescId(0),
	_isModelColumnPrepared(false),
	_selection(selection),
	_polOut(polOut),
	_ms(msPath),
	_antenna1Column(_ms, casacore::MS::columnName(casacore::MSMainEnums::ANTENNA1)),
	_antenna2Column(_ms, casacore::MS::columnName(casacore::MSMainEnums::ANTENNA2)),
	_fieldIdColumn(_ms, casacore::MS::columnName(casacore::MSMainEnums::FIELD_ID)),
	_dataDescIdColumn(_ms, casacore::MS::columnName(casacore::MSMainEnums::DATA_DESC_ID)),
	_timeColumn(_ms, casacore::MS::columnName(casacore::MSMainEnums::TIME)),
	_uvwColumn(_ms, casacore::MS::columnName(casacore::MSMainEnums::UVW)),
	_dataColumn(_ms, dataColumnName),
	_flagColumn(_ms, casacore::MS::columnName(casacore::MSMainEnums::FLAG))
{
	std::cout << "Opening " << msPath << " with contiguous MS reader.\n";
	
	_inputPolarizations = GetMSPolarizations(_ms);
 
	const casacore::IPosition shape(_dataColumn.shape(0));
	_dataArray = casacore::Array<std::complex<float>>(shape);
	_weightArray = casacore::Array<float>(shape);
	_flagArray = casacore::Array<bool>(shape);
	_bandData = MultiBandData(_ms.spectralWindow(), _ms.dataDescription());
	
	bool isWeightDefined;
	if(_ms.isColumn(casacore::MSMainEnums::WEIGHT_SPECTRUM))
	{
		_weightColumn.reset(new casacore::ROArrayColumn<float>(_ms, casacore::MS::columnName(casacore::MSMainEnums::WEIGHT_SPECTRUM)));
		isWeightDefined = _weightColumn->isDefined(0);
	} else {
		isWeightDefined = false;
	}
	_msHasWeights = false;
	if(isWeightDefined)
	{
		casacore::IPosition modelShape = _weightColumn->shape(0);
		_msHasWeights = (modelShape == shape);
	}
	if(!_msHasWeights)
	{
		_weightArray.set(1);
		std::cout << "WARNING: This measurement set has no or an invalid WEIGHT_SPECTRUM column; all visibilities are assumed to have equal weight.\n";
	}
	
	getRowRange(_ms, selection, _startRow, _endRow);
	Reset();
}

void ContiguousMS::Reset()
{
	_row = _startRow - 1;
	_time = 0.0;
	if(_selection.HasInterval())
		_timestep = _selection.IntervalStart()-1;
	else
		_timestep = -1;
	NextRow();
}

bool ContiguousMS::CurrentRowAvailable()
{
	if(_row >= _endRow)
		return false;
		
	int fieldId = _fieldIdColumn(_row);
	int a1 = _antenna1Column(_row);
	int a2 = _antenna2Column(_row);
	casacore::Vector<double> uvw = _uvwColumn(_row);
	
	while(!_selection.IsSelected(fieldId, _timestep, a1, a2, uvw)) {
		++_row;
		if(_row >= _endRow)
			return false;
		
		fieldId = _fieldIdColumn(_row);
		a1 = _antenna1Column(_row);
		a2 = _antenna2Column(_row);
		uvw = _uvwColumn(_row);
		if(_time != _timeColumn(_row))
		{
			++_timestep;
			_time = _timeColumn(_row);
		}
		
		_isMetaRead = false;
		_isDataRead = false;
		_isWeightRead = false;
		_isModelRead = false;
	}
	
	return true;
}

void ContiguousMS::NextRow()
{
	_isMetaRead = false;
	_isDataRead = false;
	_isWeightRead = false;
	_isModelRead = false;
	
	int fieldId, a1, a2;
	casacore::Vector<double> uvw;
	do {
		++_row;
		if(_row >= _endRow)
			return;
		
		fieldId = _fieldIdColumn(_row);
		a1 = _antenna1Column(_row);
		a2 = _antenna2Column(_row);
		uvw = _uvwColumn(_row);
		if(_time != _timeColumn(_row))
		{
			++_timestep;
			_time = _timeColumn(_row);
		}
	} while(!_selection.IsSelected(fieldId, _timestep, a1, a2, uvw));
}

double ContiguousMS::StartTime()
{
	return casacore::MEpoch::ROScalarColumn(_ms, casacore::MS::columnName(casacore::MS::TIME))(_startRow).getValue().get();
}

void ContiguousMS::ReadMeta(double& u, double& v, double& w, size_t& dataDescId)
{
	readMeta();
	
	casacore::Vector<double> uvwArray = _uvwColumn(_row);
	u = uvwArray(0);
	v = uvwArray(1);
	w = uvwArray(2);
	dataDescId = _dataDescId;
}

void ContiguousMS::ReadData(std::complex<float>* buffer)
{
	readMeta();
	readData();
	readWeights();
	size_t startChannel, endChannel;
	if(_selection.HasChannelRange())
	{
		startChannel = _selection.ChannelRangeStart();
		endChannel = _selection.ChannelRangeEnd();
	}
	else {
		startChannel = 0;
		endChannel = _bandData[_dataDescId].ChannelCount();
	}
	copyWeightedData(buffer,  startChannel, endChannel, _inputPolarizations, _dataArray, _weightArray, _flagArray, _polOut);
}

void ContiguousMS::prepareModelColumn()
{
	initializeModelColumn(_ms);
	
	_modelColumn.reset(new casacore::ArrayColumn<casacore::Complex>(_ms, casacore::MS::columnName(casacore::MSMainEnums::MODEL_DATA)));
	const casacore::IPosition shape(_modelColumn->shape(0));
	_modelArray = casacore::Array<std::complex<float>>(shape);
	_isModelColumnPrepared = true;
}

void ContiguousMS::ReadModel(std::complex<float>* buffer)
{
	if(!_isModelColumnPrepared)
		prepareModelColumn();
	
	readMeta();
	readModel();
	readWeights();
	size_t startChannel, endChannel;
	if(_selection.HasChannelRange())
	{
		startChannel = _selection.ChannelRangeStart();
		endChannel = _selection.ChannelRangeEnd();
	}
	else {
		startChannel = 0;
		endChannel = _bandData[_dataDescId].ChannelCount();
	}
	copyWeightedData(buffer,  startChannel, endChannel, _inputPolarizations, _modelArray, _weightArray, _flagArray, _polOut);
}

void ContiguousMS::WriteModel(size_t rowId, std::complex<float>* buffer)
{
	if(!_isModelColumnPrepared)
		prepareModelColumn();
	
	size_t dataDescId = _dataDescIdColumn(rowId);
	size_t startChannel, endChannel;
	if(_selection.HasChannelRange())
	{
		startChannel = _selection.ChannelRangeStart();
		endChannel = _selection.ChannelRangeEnd();
	}
	else {
		startChannel = 0;
		endChannel = _bandData[dataDescId].ChannelCount();
	}
	
	_modelColumn->get(rowId, _modelArray);
	reverseCopyData(_modelArray, startChannel, endChannel, _inputPolarizations, buffer, _polOut);
	_modelColumn->put(rowId, _modelArray);
}

void ContiguousMS::ReadWeights(std::complex<float>* buffer)
{
	readMeta();
	readData();
	readWeights();
	size_t startChannel, endChannel;
	if(_selection.HasChannelRange())
	{
		startChannel = _selection.ChannelRangeStart();
		endChannel = _selection.ChannelRangeEnd();
	}
	else {
		startChannel = 0;
		endChannel = _bandData[_dataDescId].ChannelCount();
	}
	copyWeights(buffer,  startChannel, endChannel, _inputPolarizations, _dataArray, _weightArray, _flagArray, _polOut);
}

void ContiguousMS::ReadWeights(float* buffer)
{
	readMeta();
	readData();
	readWeights();
	size_t startChannel, endChannel;
	if(_selection.HasChannelRange())
	{
		startChannel = _selection.ChannelRangeStart();
		endChannel = _selection.ChannelRangeEnd();
	}
	else {
		startChannel = 0;
		endChannel = _bandData[_dataDescId].ChannelCount();
	}
	copyWeights(buffer,  startChannel, endChannel, _inputPolarizations, _dataArray, _weightArray, _flagArray, _polOut);
}

void ContiguousMS::MakeMSRowToRowIdMapping(std::vector<size_t>& msToId, const MSSelection&)
{
	size_t nRow = _ms.nrow();
	msToId.resize(nRow);
	for(size_t i=0; i!=nRow; ++i)
		msToId[i] = i;
}
