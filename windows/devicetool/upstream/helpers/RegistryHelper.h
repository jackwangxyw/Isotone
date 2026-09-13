/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2012  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#pragma once

#include <string>
#include <vector>
#include <stdexcept>
#include <exception>   // Isotone modification
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// {EACD2258-FCAC-4FF4-B36D-419E924A6D79}
const GUID EQUALIZERAPO_PRE_MIX_GUID = {0xeacd2258, 0xfcac, 0x4ff4, {0xb3, 0x6d, 0x41, 0x9e, 0x92, 0x4a, 0x6d, 0x79}};
// {EC1CC9CE-FAED-4822-828A-82A81A6F018F}
const GUID EQUALIZERAPO_POST_MIX_GUID = {0xec1cc9ce, 0xfaed, 0x4822, {0x82, 0x8a, 0x82, 0xa8, 0x1a, 0x6f, 0x01, 0x8f}};

// Isotone modification: IsoAPO's own CLSIDs and registry path. The
// Equalizer APO values above are kept so an existing install can be detected.
// {F1DFFD14-9A30-45C5-BAB2-C820C7EC718F}
const GUID ISOAPO_PRE_MIX_GUID = {0xf1dffd14, 0x9a30, 0x45c5, {0xba, 0xb2, 0xc8, 0x20, 0xc7, 0xec, 0x71, 0x8f}};
// {BAF30F18-9FA2-4E55-97D9-007CEA179824}
const GUID ISOAPO_POST_MIX_GUID = {0xbaf30f18, 0x9fa2, 0x4e55, {0x97, 0xd9, 0x00, 0x7c, 0xea, 0x17, 0x98, 0x24}};

#define APP_REGPATH L"HKEY_LOCAL_MACHINE\\SOFTWARE\\IsoAPO"
#define EQUALIZERAPO_REGPATH L"HKEY_LOCAL_MACHINE\\SOFTWARE\\EqualizerAPO"
#define USER_REGPATH L"HKEY_CURRENT_USER\\SOFTWARE\\EqualizerAPO"

// Isotone modification: while a RegistryDryRun is installed, every write goes
// to it instead of the registry, and the reads that upstream's install and
// uninstall make after their own writes are answered from it first. That lets
// the unmodified install/uninstall logic run as a dry run.
class RegistryDryRun
{
public:
	virtual ~RegistryDryRun() {}
	virtual void write(const std::wstring& operation, const std::wstring& key,
		const std::wstring& valuename, const std::wstring& data) = 0;
	// Each returns true when it knows the answer, which then goes in *result.
	virtual bool keyExists(const std::wstring& key, bool* result) = 0;
	virtual bool valueExists(const std::wstring& key, const std::wstring& valuename, bool* result) = 0;
	virtual bool readValue(const std::wstring& key, const std::wstring& valuename, std::wstring* result) = 0;
	virtual bool keyEmpty(const std::wstring& key, bool* result) = 0;
};

// Isotone modification: while a RegistryLog is installed, each real write is
// reported to it once it has succeeded, so a real install can say what it changed.
class RegistryLog
{
public:
	virtual ~RegistryLog() {}
	virtual void write(const std::wstring& operation, const std::wstring& key,
		const std::wstring& valuename, const std::wstring& data) = 0;
};

class RegistryHelper
{
public:
	static RegistryDryRun* dryRun;
	static RegistryLog* log;   // Isotone modification

	static std::wstring readValue(std::wstring key, std::wstring valuename);
	static unsigned long readDWORDValue(std::wstring key, std::wstring valuename);
	static std::vector<std::wstring> readMultiValue(std::wstring key, std::wstring valuename);
	static std::vector<unsigned char> readBinaryValue(std::wstring key, std::wstring valuename);
	static void writeValue(std::wstring key, std::wstring valuename, std::wstring value);
	static void writeDWORDValue(std::wstring key, std::wstring valuename, unsigned long value);
	static void writeMultiValue(std::wstring key, std::wstring valuename, std::wstring value);
	static void writeMultiValue(std::wstring key, std::wstring valuename, std::vector<std::wstring> values);
	static void deleteValue(std::wstring key, std::wstring valuename);
	static void createKey(std::wstring key);
	static void deleteKey(std::wstring key);
	static void makeWritable(std::wstring key);
	static void takeOwnership(std::wstring key);
	static ACCESS_MASK getFileAccessForUser(std::wstring path, unsigned long rid);
	static std::vector<std::wstring> enumSubKeys(std::wstring key);
	static bool keyExists(std::wstring key);
	static bool valueExists(std::wstring key, std::wstring valuename);
	static bool keyEmpty(std::wstring key);
	static void saveToFile(std::wstring key, std::vector<std::wstring> valuenames, std::wstring filepath);
	static std::wstring getGuidString(GUID guid);
	static bool isWindowsVersionAtLeast(unsigned major, unsigned minor);
	static HKEY openKey(const std::wstring& key, REGSAM samDesired);

private:
	static std::wstring splitKey(const std::wstring& key, HKEY* rootKey);

	static unsigned long windowsVersion;
};

class RegistryException
{
public:
	RegistryException(const std::wstring& message)
		: message(message) {}

	std::wstring getMessage()
	{
		return message;
	}

private:
	std::wstring message;
};

// Isotone modification: declared at the top of a write, reports that write to
// RegistryHelper::log when the function returns without throwing.
class RegistryWriteReport
{
public:
	RegistryWriteReport(const std::wstring& operation, const std::wstring& key,
		const std::wstring& valuename, const std::wstring& data)
		: operation(operation), key(key), valuename(valuename), data(data),
		exceptions(std::uncaught_exceptions()) {}
	~RegistryWriteReport()
	{
		if (RegistryHelper::log && std::uncaught_exceptions() == exceptions)
			RegistryHelper::log->write(operation, key, valuename, data);
	}
	RegistryWriteReport(const RegistryWriteReport&) = delete;
	RegistryWriteReport& operator=(const RegistryWriteReport&) = delete;

private:
	std::wstring operation, key, valuename, data;
	int exceptions;
};
