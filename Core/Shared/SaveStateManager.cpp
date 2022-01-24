#include "stdafx.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/ZipWriter.h"
#include "Utilities/ZipReader.h"
#include "Utilities/PNGHelper.h"
#include "Shared/SaveStateManager.h"
#include "Shared/MessageManager.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Movies/MovieManager.h"
#include "EventType.h"
#include "Debugger/Debugger.h"
#include "Netplay/GameClient.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/Video/BaseVideoFilter.h"

SaveStateManager::SaveStateManager(Emulator* emu)
{
	_emu = emu;
	_lastIndex = 1;
}

string SaveStateManager::GetStateFilepath(int stateIndex)
{
	string romFile = _emu->GetRomInfo().RomFile.GetFileName();
	string folder = FolderUtilities::GetSaveStateFolder();
	string filename = FolderUtilities::GetFilename(romFile, false) + "_" + std::to_string(stateIndex) + ".mss";
	return FolderUtilities::CombinePath(folder, filename);
}

void SaveStateManager::SelectSaveSlot(int slotIndex)
{
	_lastIndex = slotIndex;
	MessageManager::DisplayMessage("SaveStates", "SaveStateSlotSelected", std::to_string(_lastIndex));
}

void SaveStateManager::MoveToNextSlot()
{
	_lastIndex = (_lastIndex % MaxIndex) + 1;
	MessageManager::DisplayMessage("SaveStates", "SaveStateSlotSelected", std::to_string(_lastIndex));
}

void SaveStateManager::MoveToPreviousSlot()
{
	_lastIndex = (_lastIndex == 1 ? SaveStateManager::MaxIndex : (_lastIndex - 1));
	MessageManager::DisplayMessage("SaveStates", "SaveStateSlotSelected", std::to_string(_lastIndex));
}

void SaveStateManager::SaveState()
{
	SaveState(_lastIndex);
}

bool SaveStateManager::LoadState()
{
	return LoadState(_lastIndex);
}

void SaveStateManager::GetSaveStateHeader(ostream &stream)
{
	uint32_t emuVersion = _emu->GetSettings()->GetVersion();
	uint32_t formatVersion = SaveStateManager::FileFormatVersion;
	stream.write("MSS", 3);
	stream.write((char*)&emuVersion, sizeof(emuVersion));
	stream.write((char*)&formatVersion, sizeof(uint32_t));

	string sha1Hash = _emu->GetHash(HashType::Sha1);
	stream.write(sha1Hash.c_str(), sha1Hash.size());

	ConsoleType consoleType = _emu->GetConsoleType();
	stream.write((char*)&consoleType, sizeof(consoleType));

	#ifndef LIBRETRO
	SaveScreenshotData(stream);
	#endif

	RomInfo romInfo = _emu->GetRomInfo();
	string romName = FolderUtilities::GetFilename(romInfo.RomFile.GetFileName(), true);
	uint32_t nameLength = (uint32_t)romName.size();
	stream.write((char*)&nameLength, sizeof(uint32_t));
	stream.write(romName.c_str(), romName.size());
}

void SaveStateManager::SaveState(ostream &stream)
{
	GetSaveStateHeader(stream);
	_emu->Serialize(stream);
}

bool SaveStateManager::SaveState(string filepath)
{
	ofstream file(filepath, ios::out | ios::binary);

	if(file) {
		_emu->Lock();
		SaveState(file);
		_emu->Unlock();
		file.close();

		_emu->ProcessEvent(EventType::StateSaved);
		return true;
	}
	return false;
}

void SaveStateManager::SaveState(int stateIndex, bool displayMessage)
{
	string filepath = SaveStateManager::GetStateFilepath(stateIndex);
	if(SaveState(filepath)) {
		if(displayMessage) {
			MessageManager::DisplayMessage("SaveStates", "SaveStateSaved", std::to_string(stateIndex));
		}
	}
}

void SaveStateManager::SaveScreenshotData(ostream& stream)
{
	PpuFrameInfo frame = _emu->GetPpuFrame();
	stream.write((char*)&frame.Width, sizeof(uint32_t));
	stream.write((char*)&frame.Height, sizeof(uint32_t));

	unsigned long compressedSize = compressBound(512*478*2);
	vector<uint8_t> compressedData(compressedSize, 0);
	compress2(compressedData.data(), &compressedSize, (const unsigned char*)frame.FrameBuffer, frame.Width*frame.Height*2, MZ_DEFAULT_LEVEL);

	uint32_t screenshotLength = (uint32_t)compressedSize;
	stream.write((char*)&screenshotLength, sizeof(uint32_t));
	stream.write((char*)compressedData.data(), screenshotLength);
}

bool SaveStateManager::GetScreenshotData(vector<uint8_t>& out, uint32_t &width, uint32_t &height, istream& stream)
{
	stream.read((char*)&width, sizeof(uint32_t));
	stream.read((char*)&height, sizeof(uint32_t));

	uint32_t screenshotLength = 0;
	stream.read((char*)&screenshotLength, sizeof(uint32_t));

	vector<uint8_t> compressedData(screenshotLength, 0);
	stream.read((char*)compressedData.data(), screenshotLength);

	out = vector<uint8_t>(width * height * 2, 0);
	unsigned long decompSize = width * height * 2;
	if(uncompress(out.data(), &decompSize, compressedData.data(), (unsigned long)compressedData.size()) == MZ_OK) {
		return true;
	}
	return false;
}

bool SaveStateManager::LoadState(istream &stream, bool hashCheckRequired)
{
	if(_emu->GetGameClient()->Connected()) {
		MessageManager::DisplayMessage("Netplay", "NetplayNotAllowed");
		return false;
	}

	char header[3];
	stream.read(header, 3);
	if(memcmp(header, "MSS", 3) == 0) {
		uint32_t emuVersion, fileFormatVersion;

		stream.read((char*)&emuVersion, sizeof(emuVersion));
		if(emuVersion > _emu->GetSettings()->GetVersion()) {
			MessageManager::DisplayMessage("SaveStates", "SaveStateNewerVersion");
			return false;
		}

		stream.read((char*)&fileFormatVersion, sizeof(fileFormatVersion));
		if(fileFormatVersion < SaveStateManager::MinimumSupportedVersion) {
			MessageManager::DisplayMessage("SaveStates", "SaveStateIncompatibleVersion");
			return false;
		} else {
			char hash[41] = {};
			stream.read(hash, 40);

			ConsoleType consoleType;
			stream.read((char*)&consoleType, sizeof(consoleType));
			if(consoleType != _emu->GetConsoleType()) {
				MessageManager::DisplayMessage("SaveStates", "SaveStateWrongSystem");
				return false;
			}
			
			#ifndef LIBRETRO
			vector<uint8_t> frameData;
			uint32_t width = 0;
			uint32_t height = 0;
			if(GetScreenshotData(frameData, width, height, stream)) {
				RenderedFrame frame(frameData.data(), width, height);
				_emu->GetVideoDecoder()->UpdateFrame(frame, true, true);
			}
			#endif

			uint32_t nameLength = 0;
			stream.read((char*)&nameLength, sizeof(uint32_t));
			
			vector<char> nameBuffer(nameLength);
			stream.read(nameBuffer.data(), nameBuffer.size());
			string romName(nameBuffer.data(), nameLength);
			
			if(!_emu->IsRunning() /*|| cartridge->GetSha1Hash() != string(hash)*/) {
				//Game isn't loaded, or CRC doesn't match
				//TODO: Try to find and load the game
				return false;
			}
		}

		//Stop any movie that might have been playing/recording if a state is loaded
		//(Note: Loading a state is disabled in the UI while a movie is playing/recording)
		_emu->GetMovieManager()->Stop();

		_emu->Deserialize(stream, fileFormatVersion);

		return true;
	}
	MessageManager::DisplayMessage("SaveStates", "SaveStateInvalidFile");
	return false;
}

bool SaveStateManager::LoadState(string filepath, bool hashCheckRequired)
{
	ifstream file(filepath, ios::in | ios::binary);
	bool result = false;

	if(file.good()) {
		_emu->Lock();
		result = LoadState(file, hashCheckRequired);
		_emu->Unlock();
		file.close();

		if(result) {
			_emu->ProcessEvent(EventType::StateLoaded);
		}
	} else {
		MessageManager::DisplayMessage("SaveStates", "SaveStateEmpty");
	}

	return result;
}

bool SaveStateManager::LoadState(int stateIndex)
{
	string filepath = SaveStateManager::GetStateFilepath(stateIndex);
	if(LoadState(filepath, false)) {
		MessageManager::DisplayMessage("SaveStates", "SaveStateLoaded", std::to_string(stateIndex));
		return true;
	}
	return false;
}

void SaveStateManager::SaveRecentGame(string romName, string romPath, string patchPath)
{
#ifndef LIBRETRO
	//Don't do this for libretro core
	string filename = FolderUtilities::GetFilename(_emu->GetRomInfo().RomFile.GetFileName(), false) + ".rgd";
	ZipWriter writer;
	writer.Initialize(FolderUtilities::CombinePath(FolderUtilities::GetRecentGamesFolder(), filename));

	std::stringstream pngStream;
	_emu->GetVideoDecoder()->TakeScreenshot(pngStream);
	writer.AddFile(pngStream, "Screenshot.png");

	std::stringstream stateStream;
	SaveStateManager::SaveState(stateStream);
	writer.AddFile(stateStream, "Savestate.mss");

	std::stringstream romInfoStream;
	romInfoStream << romName << std::endl;
	romInfoStream << romPath << std::endl;
	romInfoStream << patchPath << std::endl;
	writer.AddFile(romInfoStream, "RomInfo.txt");
	writer.Save();
#endif
}

void SaveStateManager::LoadRecentGame(string filename, bool resetGame)
{
	ZipReader reader;
	reader.LoadArchive(filename);

	stringstream romInfoStream, stateStream;
	reader.GetStream("RomInfo.txt", romInfoStream);
	reader.GetStream("Savestate.mss", stateStream);

	string romName, romPath, patchPath;
	std::getline(romInfoStream, romName);
	std::getline(romInfoStream, romPath);
	std::getline(romInfoStream, patchPath);

	try {
		if(_emu->LoadRom(romPath, patchPath)) {
			if(!resetGame) {
				auto lock = _emu->AcquireLock();
				SaveStateManager::LoadState(stateStream, false);
			}
		}
	} catch(std::exception&) { 
		_emu->Stop(true);
	}
}

int32_t SaveStateManager::GetSaveStatePreview(string saveStatePath, uint8_t* pngData)
{
	ifstream stream(saveStatePath, ios::binary);

	if(!stream) {
		return -1;
	}

	char header[3];
	stream.read(header, 3);
	if(memcmp(header, "MSS", 3) == 0) {
		uint32_t emuVersion = 0;

		stream.read((char*)&emuVersion, sizeof(emuVersion));
		if(emuVersion > _emu->GetSettings()->GetVersion()) {
			return -1;
		}

		uint32_t fileFormatVersion = 0;
		stream.read((char*)&fileFormatVersion, sizeof(fileFormatVersion));
		if(fileFormatVersion < SaveStateManager::MinimumSupportedVersion) {
			return -1;
		}

		//Skip some header fields
		stream.seekg(44, ios::cur);

		vector<uint8_t> frameData;
		uint32_t width = 0;
		uint32_t height = 0;
		if(GetScreenshotData(frameData, width, height, stream)) {
			FrameInfo baseFrameInfo;
			baseFrameInfo.Width = width;
			baseFrameInfo.Height = height;
			
			unique_ptr<BaseVideoFilter> filter(_emu->GetVideoFilter());
			filter->SetBaseFrameInfo(baseFrameInfo);
			FrameInfo frameInfo = filter->SendFrame((uint16_t*)frameData.data(), 0, nullptr);

			std::stringstream pngStream;
			PNGHelper::WritePNG(pngStream, filter->GetOutputBuffer(), frameInfo.Width, frameInfo.Height);

			string data = pngStream.str();
			memcpy(pngData, data.c_str(), data.size());

			return (int32_t)frameData.size();
		}
	}
	return -1;
}