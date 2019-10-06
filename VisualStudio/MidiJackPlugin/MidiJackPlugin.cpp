#include "stdafx.h"

namespace
{
    // Basic type aliases
    using DeviceHandle = HMIDIIN;
	using OutputDeviceHandle = HMIDIOUT;
    using DeviceID = uint32_t;

    // Utility functions for Win32/64 compatibility
#ifdef _WIN64
    DeviceID DeviceHandleToID(DeviceHandle handle)
    {
        return static_cast<DeviceID>(reinterpret_cast<uint64_t>(handle));
    }
    DeviceHandle DeviceIDToHandle(DeviceID id)
    {
        return reinterpret_cast<DeviceHandle>(static_cast<uint64_t>(id));
    }
	DeviceID OutputDeviceHandleToID(OutputDeviceHandle handle)
	{
		return static_cast<DeviceID>(reinterpret_cast<uint64_t>(handle));
	}
	OutputDeviceHandle DeviceIDToOutputHandle(DeviceID id)
	{
		return reinterpret_cast<OutputDeviceHandle>(static_cast<uint64_t>(id));
	}
#else
    DeviceID DeviceHandleToID(DeviceHandle handle)
    {
        return reinterpret_cast<DeviceID>(handle);
    }
    DeviceHandle DeviceIDToHandle(DeviceID id)
    {
        return reinterpret_cast<DeviceHandle>(id);
    }
	DeviceID DeviceHandleToID(OutputDeviceHandle handle)
	{
		return reinterpret_cast<DeviceID>(handle);
	}
	OutputDeviceHandle DeviceIDToOutputHandle(DeviceID id)
	{
		return reinterpret_cast<OutputDeviceHandle>(id);
	}
#endif

    // MIDI message storage class
    class MidiMessage
    {
        DeviceID source_;
        uint8_t status_;
        uint8_t data1_;
        uint8_t data2_;

    public:

        MidiMessage(DeviceID source, uint32_t rawData)
            : source_(source), status_(rawData), data1_(rawData >> 8), data2_(rawData >> 16)
        {
        }

        uint64_t Encode64Bit()
        {
            uint64_t ul = source_;
            ul |= (uint64_t)status_ << 32;
            ul |= (uint64_t)data1_ << 40;
            ul |= (uint64_t)data2_ << 48;
            return ul;
        }

        std::string ToString()
        {
            char temp[256];
            std::snprintf(temp, sizeof(temp), "(%X) %02X %02X %02X", source_, status_, data1_, data2_);
            return temp;
        }
    };

    // Incoming MIDI message queue
    std::queue<MidiMessage> message_queue;

    // Device handler lists
    std::list<DeviceHandle> active_handles;
    std::stack<DeviceHandle> handles_to_close;

	std::list<OutputDeviceHandle> active_output_handles;
	std::stack<OutputDeviceHandle> output_handles_to_close;
    // Mutex for resources
    std::recursive_mutex resource_lock;

	// Mutex for output queue
	std::recursive_mutex output_resource_lock;

	char _error_msg[256];

    // MIDI input callback
    static void CALLBACK MidiInProc(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
    {
        if (wMsg == MIM_DATA)
        {
            DeviceID id = DeviceHandleToID(hMidiIn);
            uint32_t raw = static_cast<uint32_t>(dwParam1);
            resource_lock.lock();
            message_queue.push(MidiMessage(id, raw));
            resource_lock.unlock();
        }
        else if (wMsg == MIM_CLOSE)
        {
            resource_lock.lock();
            handles_to_close.push(hMidiIn);
            resource_lock.unlock();
        }
    }

    // Retrieve a name of a given device.
    std::string GetDeviceName(DeviceHandle handle)
    {
        auto casted_id = reinterpret_cast<UINT_PTR>(handle);
        MIDIINCAPS caps;
        if (midiInGetDevCaps(casted_id, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            std::wstring name(caps.szPname);
            return std::string(name.begin(), name.end());
        }
        return "unknown";
    }

	std::string GetDeviceName(OutputDeviceHandle handle)
	{
		auto casted_id = reinterpret_cast<UINT_PTR>(handle);
		MIDIOUTCAPS caps;
		if (midiOutGetDevCaps(casted_id, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
			std::wstring name(caps.szPname);
			return std::string(name.begin(), name.end());
		}
		return "unknown";
	}

    // Open a MIDI device with a given index.
    void OpenDevice(unsigned int index)
    {
        static const DWORD_PTR callback = reinterpret_cast<DWORD_PTR>(MidiInProc);
        DeviceHandle handle;

        if (midiInOpen(&handle, index, callback, NULL, CALLBACK_FUNCTION) == MMSYSERR_NOERROR)
        {
            if (midiInStart(handle) == MMSYSERR_NOERROR)
            {
                resource_lock.lock();
                active_handles.push_back(handle);
                resource_lock.unlock();
            }
            else
            {
                midiInClose(handle);
            }
        }
    }

	//Open a MIDI output device
	void OpenOutputDevice(unsigned int index)
	{
		OutputDeviceHandle output_handle;
		//static const DWORD_PTR output_callback = reinterpret_cast<DWORD_PTR>();
		if (midiOutOpen(&output_handle, index, NULL, NULL, CALLBACK_NULL) != MMSYSERR_NOERROR)
		{
			midiOutClose(output_handle);
		} else {
			std::lock_guard<std::recursive_mutex> lock(output_resource_lock);
			active_output_handles.push_back(output_handle);
		}
	}

    // Close a given handler.
    void CloseDevice(DeviceHandle handle)
    {
        midiInClose(handle);

        resource_lock.lock();
        active_handles.remove(handle);
        resource_lock.unlock();
    }

	void CloseOutputDevice(OutputDeviceHandle handle)
	{
		midiOutClose(handle);

		output_resource_lock.lock();
		active_output_handles.remove(handle);
		output_resource_lock.unlock();
	}

    // Open the all devices.
    void OpenAllDevices()
    {
        int device_count = midiInGetNumDevs();
        for (int i = 0; i < device_count; i++) OpenDevice(i);

		int output_device_count = midiOutGetNumDevs();
		for (int i = 0; i < output_device_count; ++i) OpenOutputDevice(i);
    }

    // Refresh device handlers
    void RefreshDevices()
    {
        resource_lock.lock();

        // Close disconnected handlers.
        while (!handles_to_close.empty()) {
            CloseDevice(handles_to_close.top());
            handles_to_close.pop();
        }

        // Try open all devices to detect newly connected ones.
        OpenAllDevices();

        resource_lock.unlock();
    }

	void RefreshOutputDevices()
	{
		std::lock_guard<std::recursive_mutex> lock(output_resource_lock);
		while (!output_handles_to_close.empty()) {
			CloseOutputDevice(output_handles_to_close.top());
			output_handles_to_close.pop();
		}
		OpenAllDevices();
	}

    // Close the all devices.
    void CloseAllDevices()
    {
        resource_lock.lock();
        while (!active_handles.empty())
            CloseDevice(active_handles.front());
        resource_lock.unlock();
		
		output_resource_lock.lock();
		while (!active_output_handles.empty())
			CloseOutputDevice(active_output_handles.front());
		output_resource_lock.unlock();
    }
}

// Exported functions

#define EXPORT_API extern "C" __declspec(dllexport)

// Counts the number of endpoints.
EXPORT_API int MidiJackCountEndpoints()
{
    return static_cast<int>(active_handles.size());
}

EXPORT_API int MidiJackCountOutputEndpoints()
{
	return static_cast<int>(active_output_handles.size());
}

// Get the unique ID of an endpoint.
EXPORT_API uint32_t MidiJackGetEndpointIDAtIndex(int index)
{
    auto itr = active_handles.begin();
    std::advance(itr, index);
    return DeviceHandleToID(*itr);
}

EXPORT_API uint32_t MidiJackGetOutputEndpointIDAtIndex(int index)
{
	auto itr = active_output_handles.begin();
	std::advance(itr, index);
	return OutputDeviceHandleToID(*itr);
}

// Get the name of an endpoint.
EXPORT_API const char* MidiJackGetEndpointName(uint32_t id)
{
    auto handle = DeviceIDToHandle(id);
    static std::string buffer;
    buffer = GetDeviceName(handle);
    return buffer.c_str();
}

EXPORT_API const char* MidiJackGetOutputEndpointName(uint32_t id)
{
	auto handle = DeviceIDToOutputHandle(id);
	static std::string buffer;
	buffer = GetDeviceName(handle);
	return buffer.c_str();
}


// Retrieve and erase an MIDI message data from the message queue.
EXPORT_API uint64_t MidiJackDequeueIncomingData()
{
    RefreshDevices();

    if (message_queue.empty()) return 0;

    resource_lock.lock();
    auto msg = message_queue.front();
    message_queue.pop();
    resource_lock.unlock();

    return msg.Encode64Bit();
}

EXPORT_API bool MidiJackSendMessage(uint32_t id, uint32_t rawMsg)
{
	auto itr = active_output_handles.begin();
	std::advance(itr, id);
	OutputDeviceHandle handle = *itr;
	auto result = midiOutShortMsg(handle, rawMsg);
	if (result != MMSYSERR_NOERROR) {
		midiOutGetErrorTextA(result, _error_msg, 256);
	}
	return result == MMSYSERR_NOERROR;
}

EXPORT_API const char* MidiJackGetLastError()
{
	return _error_msg;
}