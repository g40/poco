//
// ODBCStatementImpl.cpp
//
// $Id: //poco/1.3/Data/ODBC/src/ODBCStatementImpl.cpp#7 $
//
// Library: ODBC
// Package: ODBC
// Module:  ODBCStatementImpl
//
// Copyright (c) 2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
// 
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//


#include "Poco/Data/ODBC/ODBCStatementImpl.h"
#include "Poco/Data/ODBC/Handle.h"
#include "Poco/Data/ODBC/Utility.h"
#include "Poco/Data/ODBC/ODBCException.h"
#include "Poco/Data/AbstractPrepare.h"
#include <sql.h>


namespace Poco {
namespace Data {
namespace ODBC {


const std::string ODBCStatementImpl::INVALID_CURSOR_STATE = "24000";


ODBCStatementImpl::ODBCStatementImpl(SessionImpl& rSession):
	_rSession(rSession),
	_stmt(_rSession.dbc()),
	_stepCalled(false),
	_nextResponse(0)
{
	if (_rSession.getFeature("autoBind"))
	{
		SQLSetStmtAttr(_stmt, 
			SQL_ATTR_PARAM_BIND_TYPE, 
			(SQLPOINTER) SQL_PARAM_BIND_BY_COLUMN, 
			0);
	}
	else
	{
		SQLSetStmtAttr(_stmt, 
			SQL_ATTR_ROW_ARRAY_SIZE, 
			(SQLPOINTER) 1, 
			0);
	}
}


ODBCStatementImpl::~ODBCStatementImpl()
{
	ColumnPtrVec::iterator it = _columnPtrs.begin();
	ColumnPtrVec::iterator itEnd = _columnPtrs.end();
	for(; it != itEnd; ++it) delete *it;
}


void ODBCStatementImpl::compileImpl()
{
	_stepCalled   = false;
	_nextResponse = 0;

	std::string statement(toString());
	if (statement.empty())
		throw ODBCException("Empty statements are illegal");

	Preparation::DataExtraction ext = _rSession.getFeature("autoExtract") ? 
		Preparation::DE_BOUND : Preparation::DE_MANUAL;
	
	std::size_t maxFieldSize = AnyCast<std::size_t>(_rSession.getProperty("maxFieldSize"));
	_pPreparation = new Preparation(_stmt, 
		statement, 
		maxFieldSize,
		ext);

	Binder::ParameterBinding bind = _rSession.getFeature("autoBind") ? 
		Binder::PB_IMMEDIATE : Binder::PB_AT_EXEC;

	_pBinder = new Binder(_stmt, bind);
	_pExtractor = new Extractor(_stmt, *_pPreparation);

	bool dataAvailable = hasData();
	if (dataAvailable && !extractions().size()) 
	{
		fillColumns();
		makeExtractors(columnsReturned());
	}

	if (Preparation::DE_BOUND == ext && dataAvailable)
	{
		std::size_t pos = 0;
		Extractions& extracts = extractions();
		Extractions::iterator it    = extracts.begin();
		Extractions::iterator itEnd = extracts.end();
		for (; it != itEnd; ++it)
		{
			AbstractPrepare* pAP = (*it)->createPrepareObject(_pPreparation, pos);
			pAP->prepare();
			pos += (*it)->numOfColumnsHandled();
			delete pAP;
		}
	}
}


bool ODBCStatementImpl::canBind() const
{
	if (!bindings().empty())
		return (*bindings().begin())->canBind();

	return false;
}


void ODBCStatementImpl::bindImpl()
{
	clear();
	Bindings& binds = bindings();
	if (!binds.empty())
	{
		std::size_t pos = 0;

		Bindings::iterator it    = binds.begin();
		Bindings::iterator itEnd = binds.end();
		for (; it != itEnd && (*it)->canBind(); ++it)
		{
			(*it)->bind(pos);
			pos += (*it)->numOfColumnsHandled();
		}
	}

	SQLRETURN rc = SQLExecute(_stmt);

	if (SQL_NEED_DATA == rc) putData();
	else checkError(rc, "SQLExecute()");
}


void ODBCStatementImpl::putData()
{
	SQLPOINTER pParam = 0;
	SQLRETURN rc = SQLParamData(_stmt, &pParam);

	do
	{
		poco_assert_dbg (pParam);
		
		SQLINTEGER dataSize = (SQLINTEGER) _pBinder->dataSize(pParam);

		if (Utility::isError(SQLPutData(_stmt, pParam, dataSize))) 
			throw StatementException(_stmt, "SQLPutData()");
	}while (SQL_NEED_DATA == (rc = SQLParamData(_stmt, &pParam)));

	checkError(rc, "SQLParamData()");
}


void ODBCStatementImpl::clear()
{
	SQLRETURN rc = SQLCloseCursor(_stmt);
	_stepCalled = false;
	if (Utility::isError(rc))
	{
		StatementError err(_stmt);
		bool ignoreError = false;

		const StatementDiagnostics& diagnostics = err.diagnostics();
		//ignore "Invalid cursor state" error 
		//(returned by 3.x drivers when cursor is not opened)
		for (int i = 0; i < diagnostics.count(); ++i)
		{
			if (ignoreError = 
				(INVALID_CURSOR_STATE == std::string(diagnostics.sqlState(i))))
			{
				break;
			}
		}
		
		if (!ignoreError)
			throw StatementException(_stmt, "SQLCloseCursor()");
	}
}


bool ODBCStatementImpl::hasNext()
{
	if (hasData())
	{
		if (_stepCalled) 
			return _stepCalled = nextRowReady();

		_stepCalled = true;
		_nextResponse = SQLFetch(_stmt);

		if (!nextRowReady())
			return false;
		else
		if (Utility::isError(_nextResponse))
			checkError(_nextResponse, "SQLFetch()");

		return true;
	}

	return false;
}


void ODBCStatementImpl::next()
{
	if (nextRowReady())
	{
		poco_assert (columnsExtracted() == _pPreparation->columns());

		Extractions& extracts = extractions();
		Extractions::iterator it    = extracts.begin();
		Extractions::iterator itEnd = extracts.end();
		std::size_t pos = 0;
		for (; it != itEnd; ++it)
		{
			(*it)->extract(pos);
			pos += (*it)->numOfColumnsHandled();
		}
		_stepCalled = false;
	}
	else
	{
		throw StatementException(_stmt,
			std::string("Iterator Error: trying to access the next value"));
	}
}


std::string ODBCStatementImpl::nativeSQL()
{
	std::string statement = toString();

	//Hopefully, double the original statement length is enough.
	//If it is not, the total available length is indicated in the retlen parameter,
	//which is in turn used to resize the buffer and request the native SQL again.
	SQLINTEGER length = (SQLINTEGER) statement.size() * 2;

	char* pNative = 0;
	SQLINTEGER retlen = length;
	do
	{
		delete [] pNative;
		pNative = new char[retlen];
		std::memset(pNative, 0, retlen);
		length = retlen;
		if (Utility::isError(SQLNativeSql(_rSession.dbc(),
			(POCO_SQLCHAR*) statement.c_str(),
			(SQLINTEGER) statement.size(),
			(POCO_SQLCHAR*) pNative,
			length,
			&retlen)))
		{
			delete [] pNative;
			throw ConnectionException(_rSession.dbc(), "SQLNativeSql()");
		}
		++retlen;//accomodate for terminating '\0'
	}while (retlen > length);

	std::string sql(pNative);
	delete [] pNative;
	return sql;
}


void ODBCStatementImpl::checkError(SQLRETURN rc, const std::string& msg)
{
	if (SQL_NO_DATA == rc) return;

	if (Utility::isError(rc))
	{
		std::ostringstream os; 	 
	    os << std::endl << "Requested SQL statement: " << toString() << std::endl; 	 
	    os << "Native SQL statement: " << nativeSQL() << std::endl; 	 
	    std::string str(msg); str += os.str();

		throw StatementException(_stmt, str);
	}
}


void ODBCStatementImpl::fillColumns()
{
	Poco::UInt32 colCount = columnsReturned();

	for (int i = 0; i < colCount; ++i)
		_columnPtrs.push_back(new ODBCColumn(_stmt, i));
}


} } } // namespace Poco::Data::ODBC
