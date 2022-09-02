#include "stdafx.h"
#include "PCE/PceCdRom.h"
#include "PCE/PceConsole.h"
#include "PCE/PceMemoryManager.h"
#include "PCE/PceCdAudioPlayer.h"
#include "PCE/PceTypes.h"
#include "Shared/Emulator.h"
#include "Shared/MessageManager.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/Serializer.h"

using namespace ScsiSignal;

PceCdRom::PceCdRom(Emulator* emu, PceConsole* console, DiscInfo& disc) : _disc(disc), _scsi(console, this, _disc), _adpcm(console, emu, this, &_scsi), _audioPlayer(emu, this, _disc), _audioFader(console)
{
	_emu = emu;
	_console = console;

	_emu->GetSoundMixer()->RegisterAudioProvider(&_audioPlayer);
	_emu->GetSoundMixer()->RegisterAudioProvider(&_adpcm);
}

PceCdRom::~PceCdRom()
{
	_emu->GetSoundMixer()->UnregisterAudioProvider(&_audioPlayer);
	_emu->GetSoundMixer()->UnregisterAudioProvider(&_adpcm);
}

void PceCdRom::SetIrqSource(PceCdRomIrqSource src)
{
	//LogDebug("Set IRQ source: " + HexUtilities::ToHex((uint8_t)src));
	if((_state.ActiveIrqs & (uint8_t)src) == 0) {
		_state.ActiveIrqs |= (uint8_t)src;
		UpdateIrqState();
	}
}

void PceCdRom::ClearIrqSource(PceCdRomIrqSource src)
{
	//LogDebug("Clear IRQ source: " + HexUtilities::ToHex((uint8_t)src));
	if(_state.ActiveIrqs & (uint8_t)src) {
		_state.ActiveIrqs &= ~(uint8_t)src;
		UpdateIrqState();
	}
}

void PceCdRom::UpdateIrqState()
{
	if((_state.EnabledIrqs & _state.ActiveIrqs) != 0) {
		_console->GetMemoryManager()->SetIrqSource(PceIrqSource::Irq2);
	} else {
		_console->GetMemoryManager()->ClearIrqSource(PceIrqSource::Irq2);
	}
}

void PceCdRom::Write(uint16_t addr, uint8_t value)
{
	switch(addr & 0x3FF) {
		case 0x00: //SCSI Control
			_scsi.SetSignalValue(Sel, true);
			_scsi.UpdateState();
			_scsi.SetSignalValue(Sel, false);
			_scsi.UpdateState();
			break;

		case 0x01: //CDC/SCSI Command
			_scsi.SetDataPort(value);
			_scsi.UpdateState();
			break;

		case 0x02: //ACK
			_scsi.SetSignalValue(Ack, (value & 0x80) != 0);
			_scsi.UpdateState();

			_state.EnabledIrqs = value & 0x7C;
			UpdateIrqState();
			break;

		case 0x03: break; //BRAM lock, CD Status (readonly)

		case 0x04: {
			//Reset
			bool reset = (value & 0x02) != 0;
			_scsi.SetSignalValue(Rst, reset);
			_scsi.UpdateState();
			if(reset) {
				//Clear enabled IRQs flags for SCSI drive (SubChannel + DataTransferDone + DataTransferReady)
				_state.EnabledIrqs &= 0x8F;
				UpdateIrqState();
			}
			break;
		}

		case 0x05:
		case 0x06:
			//Latch CD data
			break;

		case 0x07:
			//BRAM unlock
			if((value & 0x80) != 0) {
				_state.BramLocked = false;
			}
			break;

		case 0x08: case 0x09: case 0x0A: case 0x0B:
		case 0x0C: case 0x0D: case 0x0E:
			_adpcm.Write(addr, value);
			break;

		case 0x0F:
			_audioFader.Write(value);
			break;
	}
}

uint8_t PceCdRom::Read(uint16_t addr)
{
	switch(addr & 0x3FF) {
		case 0x00: return _scsi.GetStatus();
		case 0x01: return _scsi.GetDataPort();
		case 0x02: return _state.EnabledIrqs | (_scsi.CheckSignal(Ack) ? 0x80 : 0);
		case 0x03:
			//TODO BramLocked doesn't do anything
			_state.BramLocked = true;
			_state.ReadRightChannel = !_state.ReadRightChannel;

			return (
				_state.ActiveIrqs |
				(_state.ReadRightChannel ? 0 : 0x02)
			);

		case 0x04: return _scsi.CheckSignal(Rst) ? 0x02 : 0;

		case 0x05: return (uint8_t)(_state.ReadRightChannel ? _audioPlayer.GetRightSample() : _audioPlayer.GetLeftSample());
		case 0x06: return (uint8_t)((_state.ReadRightChannel ? _audioPlayer.GetRightSample() : _audioPlayer.GetLeftSample()) >> 8);
			
		case 0x07: return _state.BramLocked ? 0 : 0x80;

		case 0x08: {
			uint8_t val = _scsi.GetDataPort();
			if(_scsi.CheckSignal(Req) && _scsi.CheckSignal(Io) && !_scsi.CheckSignal(Cd)) {
				_scsi.SetAckWithAutoClear();
				_scsi.UpdateState();
			}
			return val;
		}

		case 0x09: case 0x0A: case 0x0B:
		case 0x0C: case 0x0D: case 0x0E:
			return _adpcm.Read(addr);

		case 0xC0: case 0xC1: case 0xC2: case 0xC3: 
			if(_emu->GetSettings()->GetPcEngineConfig().CdRomType == PceCdRomType::CdRom) {
				return 0xFF;
			} else {
				constexpr uint8_t superCdRomSignature[4] = { 0x00, 0xAA, 0x55, 0x03 };
				return superCdRomSignature[addr & 0x03];
			}

		default:
			LogDebug("Read unknown CDROM register: " + HexUtilities::ToHex(addr));
			break;
	}

	return 0xFF;
}

void PceCdRom::Serialize(Serializer& s)
{
	SV(_state.ActiveIrqs);
	SV(_state.BramLocked);
	SV(_state.EnabledIrqs);
	SV(_state.ReadRightChannel);

	SV(_scsi);
	SV(_adpcm);
	SV(_audioPlayer);
	SV(_audioFader);
}
