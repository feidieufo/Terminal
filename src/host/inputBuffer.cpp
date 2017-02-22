/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"
#include "inputBuffer.hpp"
#include "dbcs.h"
#include "stream.h"

#include <algorithm>

#define INPUT_BUFFER_DEFAULT_INPUT_MODE (ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT)

// Routine Description:
// - This routine creates an input buffer.  It allocates the circular buffer and initializes the information fields.
// Arguments:
// - cEvents - The default size of the circular buffer (in INPUT_RECORDs)
// Return Value:
InputBuffer::InputBuffer(_In_ ULONG cEvents)
{
    if (0 == cEvents)
    {
        cEvents = DEFAULT_NUMBER_OF_EVENTS;
    }

    // Allocate memory for circular buffer.
    ULONG uTemp;
    if (FAILED(DWordAdd(cEvents, 1, &uTemp)) || FAILED(DWordMult(sizeof(INPUT_RECORD), uTemp, &uTemp)))
    {
        cEvents = DEFAULT_NUMBER_OF_EVENTS;
    }

    NTSTATUS Status = STATUS_SUCCESS;
    this->InputWaitEvent = g_hInputEvent.get();

    // TODO: MSFT:8805366 Is this if block still necessary?
    if (!NT_SUCCESS(Status))
    {
        THROW_NTSTATUS(Status);
    }

    // initialize buffer header
    this->InputMode = INPUT_BUFFER_DEFAULT_INPUT_MODE;
    this->ImeMode.Disable = FALSE;
    this->ImeMode.Unavailable = FALSE;
    this->ImeMode.Open = FALSE;
    this->ImeMode.ReadyConversion = FALSE;
    this->ImeMode.InComposition = FALSE;

    ZeroMemory(&this->ReadConInpDbcsLeadByte, sizeof(INPUT_RECORD));
    ZeroMemory(&this->WriteConInpDbcsLeadByte, sizeof(INPUT_RECORD));
}

// Routine Description:
// - This routine resets the input buffer information fields to their initial values.
// Arguments:
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
void InputBuffer::ReinitializeInputBuffer()
{
    ResetEvent(this->InputWaitEvent);

    this->InputMode = INPUT_BUFFER_DEFAULT_INPUT_MODE;
    _storage.clear();
}

// Routine Description:
// - This routine frees the resources associated with an input buffer.
// Arguments:
// - None
// Return Value:
InputBuffer::~InputBuffer()
{
    // TODO: MSFT:8805366 check for null before trying to close this
    // and check that it needs to be closing it in the first place.
    CloseHandle(this->InputWaitEvent);
}

// Routine Description:
// - This routine returns the number of events in the input buffer.
// Arguments:
// - None
// Return Value:
// The number of events currently in the input buffer.
// Note:
// - The console lock must be held when calling this routine.
size_t InputBuffer::GetNumberOfReadyEvents()
{
    return _storage.size();
}

// Routine Description:
// - This routine removes all but the key events from the buffer.
// Arguments:
// Return Value:
// - S_OK on success, other HRESULTS otherwise.
// Note:
// - The console lock must be held when calling this routine.
HRESULT InputBuffer::FlushAllButKeys()
{
    try
    {
        std::deque<INPUT_RECORD> keyRecords;
        for(auto it = _storage.begin(); it != _storage.end(); ++it)
        {
            if (it->EventType == KEY_EVENT)
            {
                keyRecords.push_back(*it);
            }
        }
        _storage.swap(keyRecords);
        return S_OK;
    }
    CATCH_RETURN();
}

// Routine Description:
// - This routine empties the input buffer
// Arguments:
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
void InputBuffer::FlushInputBuffer()
{
    _storage.clear();
    ResetEvent(InputWaitEvent);
}

// TODO docs
// Routine Description:
// - This routine reads from a buffer.  It does the actual circular buffer manipulation.
// Arguments:
// - Buffer - buffer to read into
// - Length - length of buffer in events
// - EventsRead - where to store number of events read
// - Peek - if TRUE, don't remove data from buffer, just copy it.
// - StreamRead - if TRUE, events with repeat counts > 1 are returned as multiple events.  also, EventsRead == 1.
// - ResetWaitEvent - on exit, TRUE if buffer became empty.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS InputBuffer::_ReadBuffer(_Out_writes_to_(Length, *EventsRead) PINPUT_RECORD Buffer,
                                        _In_ ULONG Length,
                                        _Out_ PULONG EventsRead,
                                        _In_ BOOL Peek,
                                        _In_ BOOL StreamRead,
                                        _Out_ PBOOL ResetWaitEvent,
                                        _In_ BOOLEAN Unicode)
{
    std::deque<INPUT_RECORD> outRecords;
    size_t eventsRead;
    bool resetWaitEvent;

    // call inner func
    LOG_IF_FAILED(_ReadBuffer(outRecords,
                              Length,
                              eventsRead,
                              !!Peek,
                              !!StreamRead,
                              resetWaitEvent,
                              !!Unicode));

    // move data back to original vars
    *ResetWaitEvent = !!resetWaitEvent;
    LOG_IF_FAILED(SizeTToULong(eventsRead, EventsRead));

    // copy events over to the array
    ASSERT(outRecords.size() < Length);
    size_t currentIndex = 0;
    while (!outRecords.empty())
    {
        Buffer[currentIndex] = outRecords.front();
        outRecords.pop_front();
        ++currentIndex;
    }

    return STATUS_SUCCESS;

}

// TODO docs
HRESULT InputBuffer::_ReadBuffer(std::deque<INPUT_RECORD>& outRecords,
                                       const size_t readCount,
                                       size_t& eventsRead,
                                       const bool peek,
                                       const bool streamRead,
                                       bool& resetWaitEvent,
                                       const bool unicode)
{
    try
    {
        resetWaitEvent = false;

        // TODO is this still necessary? does streamRead actually do anything?
        // legacy comment below:
        //
        // If StreamRead, just return one record. If repeat count is greater than
        // one, just decrement it. The repeat count is > 1 if more than one event
        // of the same type was merged. We need to expand them back to individual
        // events here.
        if (streamRead && _storage.back().EventType == KEY_EVENT)
        {
            ASSERT(readCount == 1);
            ASSERT(!_storage.empty());
            outRecords.push_back(_storage.front());
            _storage.pop_front();
            if (_storage.empty())
            {
                resetWaitEvent = true;
            }
            eventsRead = 1;
            return STATUS_SUCCESS;
        }

        // we need another var to keep track of how many we've read
        // because dbcs records count for two when we aren't doing a
        // unicode read but the eventsRead count should return the number
        // of events actually put into outRecords.
        size_t virtualReadCount = 0;
        while (!_storage.empty() && virtualReadCount < readCount)
        {
            outRecords.push_back(_storage.front());
            ++virtualReadCount;
            if (!unicode)
            {
                if (_storage.front().EventType == KEY_EVENT &&
                    IsCharFullWidth(_storage.front().Event.KeyEvent.uChar.UnicodeChar))
                {
                    ++virtualReadCount;
                }
            }
            _storage.pop_front();
        }

        // the amount of events that were actually read
        eventsRead = outRecords.size();

        // signal if we emptied the buffer
        if (_storage.empty())
        {
            resetWaitEvent = true;
        }

        // copy the events back if we were supposed to peek
        if (peek)
        {
            for (auto it = outRecords.crbegin(); it != outRecords.crend(); ++it)
            {
                _storage.push_front(*it);
            }
        }
        return S_OK;
    }
    CATCH_RETURN();
}

// Routine Description:
// - This routine reads from the input buffer.
// Arguments:
// - pInputRecord - Buffer to read into.
// - pcLength - On input, number of events to read.  On output, number of events read.
// - fPeek - If TRUE, copy events to pInputRecord but don't remove them from the input buffer.
// - fWaitForData - if TRUE, wait until an event is input.  if FALSE, return immediately
// - fStreamRead - if TRUE, events with repeat counts > 1 are returned as multiple events.  also, EventsRead == 1.
// - pHandleData - Pointer to handle data structure.  This parameter is optional if fWaitForData is false.
// - pConsoleMsg - if called from dll (not InputThread), points to api message.  this parameter is used for wait block processing.
// - pfnWaitRoutine - Routine to call when wait is woken up.
// - pvWaitParameter - Parameter to pass to wait routine.
// - cbWaitParameter - Length of wait parameter.
// - fWaitBlockExists - TRUE if wait block has already been created.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS InputBuffer::ReadInputBuffer(_Out_writes_(*pcLength) PINPUT_RECORD pInputRecord,
                                            _Inout_ PDWORD pcLength,
                                            _In_ BOOL const fPeek,
                                            _In_ BOOL const fWaitForData,
                                            _In_ BOOL const fStreamRead,
                                            _In_ INPUT_READ_HANDLE_DATA* pHandleData,
                                            _In_opt_ PCONSOLE_API_MSG pConsoleMsg,
                                            _In_opt_ ConsoleWaitRoutine pfnWaitRoutine,
                                            _In_reads_bytes_opt_(cbWaitParameter) PVOID pvWaitParameter,
                                            _In_ ULONG const cbWaitParameter,
                                            _In_ BOOLEAN const fWaitBlockExists,
                                            _In_ BOOLEAN const fUnicode)
{
    NTSTATUS Status;
    if (_storage.empty())
    {
        if (!fWaitForData)
        {
            *pcLength = 0;
            return STATUS_SUCCESS;
        }

        pHandleData->IncrementReadCount();
        Status = WaitForMoreToRead(pConsoleMsg, pfnWaitRoutine, pvWaitParameter, cbWaitParameter, fWaitBlockExists);
        if (!NT_SUCCESS(Status))
        {
            if (Status != CONSOLE_STATUS_WAIT)
            {
                // WaitForMoreToRead failed, restore ReadCount and bail out
                pHandleData->DecrementReadCount();
            }

            *pcLength = 0;
            return Status;
        }
    }

    // read from buffer
    ULONG EventsRead;
    BOOL ResetWaitEvent;
    Status = _ReadBuffer(pInputRecord, *pcLength, &EventsRead, fPeek, fStreamRead, &ResetWaitEvent, fUnicode);
    if (ResetWaitEvent)
    {
        ResetEvent(InputWaitEvent);
    }

    *pcLength = EventsRead;
    return Status;
}

bool InputBuffer::_CoalesceMouseMovedEvents(_In_ std::deque<INPUT_RECORD>& inRecords)
{
    _ASSERT(inRecords.size() == 1);
    const INPUT_RECORD& firstInRecord = inRecords.front();
    INPUT_RECORD& lastStoredRecord = _storage.back();
    if (firstInRecord.EventType == MOUSE_EVENT &&
        firstInRecord.Event.MouseEvent.dwEventFlags == MOUSE_MOVED &&
        lastStoredRecord.EventType == MOUSE_EVENT &&
        lastStoredRecord.Event.MouseEvent.dwEventFlags == MOUSE_MOVED)
    {
        lastStoredRecord.Event.MouseEvent.dwMousePosition.X = firstInRecord.Event.MouseEvent.dwMousePosition.X;
        lastStoredRecord.Event.MouseEvent.dwMousePosition.Y = firstInRecord.Event.MouseEvent.dwMousePosition.Y;
        inRecords.pop_front();
        return true;
    }
    return false;
}

bool InputBuffer::_CoalesceRepeatedKeyPressEvents(_In_ std::deque<INPUT_RECORD>& inRecords)
{
    _ASSERT(inRecords.size() == 1);
    const INPUT_RECORD& firstInRecord = inRecords.front();
    INPUT_RECORD& lastStoredRecord = _storage.back();
    // both records need to be key down events
    if (firstInRecord.EventType == KEY_EVENT &&
        firstInRecord.Event.KeyEvent.bKeyDown &&
        lastStoredRecord.EventType == KEY_EVENT &&
        lastStoredRecord.Event.KeyEvent.bKeyDown &&
        !IsCharFullWidth(firstInRecord.Event.KeyEvent.uChar.UnicodeChar))
    {
        const KEY_EVENT_RECORD inKeyEventRecord = firstInRecord.Event.KeyEvent;
        const KEY_EVENT_RECORD lastKeyEventRecord = lastStoredRecord.Event.KeyEvent;
        bool sameKey = false;
        // ime key check
        if (IsFlagSet(inKeyEventRecord.dwControlKeyState, NLS_IME_CONVERSION) &&
            inKeyEventRecord.uChar.UnicodeChar == lastKeyEventRecord.uChar.UnicodeChar &&
            inKeyEventRecord.dwControlKeyState == lastKeyEventRecord.dwControlKeyState)
        {
            sameKey = true;
        }
        // other key events check
        else if (inKeyEventRecord.wVirtualScanCode == lastKeyEventRecord.wVirtualScanCode &&
                 inKeyEventRecord.uChar.UnicodeChar == lastKeyEventRecord.uChar.UnicodeChar &&
                 inKeyEventRecord.dwControlKeyState == lastKeyEventRecord.dwControlKeyState)
        {
            sameKey = true;
        }
        if (sameKey)
        {
            lastStoredRecord.Event.KeyEvent.wRepeatCount += firstInRecord.Event.KeyEvent.wRepeatCount;
            inRecords.pop_front();
            return true;
        }
    }
    return false;
}

// TODO redo docs
// Routine Description:
// - This routine writes to a buffer.  It does the actual circular buffer manipulation.
// Arguments:
// - Buffer - buffer to write from
// - Length - length of buffer in events
// - EventsWritten - where to store number of events written.
// - SetWaitEvent - on exit, TRUE if buffer became non-empty.
// Return Value:
// - ERROR_BROKEN_PIPE - no more readers.
// Note:
// - The console lock must be held when calling this routine.
HRESULT InputBuffer::_WriteBuffer(_In_ std::deque<INPUT_RECORD>& inRecords,
                                        _Out_ size_t& eventsWritten,
                                        _Out_ bool& setWaitEvent)
{
    try
    {
        eventsWritten = 0;
        setWaitEvent = false;
        bool initiallyEmptyQueue = _storage.empty();
        if (inRecords.size() == 1 && !_storage.empty())
        {
            bool coalesced = false;
            // this looks kinda weird but we don't want to coalesce a
            // mouse event and then try to coalesce a key event right after.
            if (_CoalesceMouseMovedEvents(inRecords))
            {
                coalesced = true;
            }
            else if (_CoalesceRepeatedKeyPressEvents(inRecords))
            {
                coalesced = true;
            }
            if (coalesced)
            {
                eventsWritten = 1;
                return S_OK;
            }
        }
        // add all input records to the storage queue
        while (!inRecords.empty())
        {
            _storage.push_back(inRecords.front());
            inRecords.pop_front();
            ++eventsWritten;
        }
        if (initiallyEmptyQueue && !_storage.empty())
        {
            setWaitEvent = true;
        }
        return S_OK;
    }
    CATCH_RETURN();
}

// TODO docs
// Routine Description:
// - This routine processes special characters in the input stream.
// Arguments:
// - Console - Pointer to console structure.
// - InputEvent - Buffer to write from.
// - nLength - Number of events to write.
// Return Value:
// - Number of events to write after special characters have been stripped.
// Note:
// - The console lock must be held when calling this routine.
HRESULT InputBuffer::_HandleConsoleSuspensionEvents(_In_ std::deque<INPUT_RECORD>& records)
{
    try
    {
        std::deque<INPUT_RECORD> outRecords;
        for (size_t i = 0; i < records.size(); ++i)
        {
            INPUT_RECORD currEvent = records[i];
            if (currEvent.EventType == KEY_EVENT && currEvent.Event.KeyEvent.bKeyDown)
            {
                if (IsFlagSet(g_ciConsoleInformation.Flags, CONSOLE_SUSPENDED) &&
                    !IsSystemKey(currEvent.Event.KeyEvent.wVirtualKeyCode))
                {
                    UnblockWriteConsole(CONSOLE_OUTPUT_SUSPENDED);
                    continue;
                }
                else if (IsFlagSet(InputMode, ENABLE_LINE_INPUT) &&
                        currEvent.Event.KeyEvent.wVirtualKeyCode == VK_PAUSE || IsPauseKey(&currEvent.Event.KeyEvent))
                {
                    SetFlag(g_ciConsoleInformation.Flags, CONSOLE_SUSPENDED);
                    continue;
                }
            }
            outRecords.push_back(currEvent);
        }
        records.swap(outRecords);
        return S_OK;
    }
    CATCH_RETURN();
}

// TODO docs
// Routine Description:
// -  This routine writes to the beginning of the input buffer.
// Arguments:
// - pInputRecord - Buffer to write from.
// - cInputRecords - On input, number of events to write.  On output, number of events written.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
NTSTATUS InputBuffer::PrependInputBuffer(_In_ INPUT_RECORD* pInputRecord, _Inout_ DWORD* const pcLength)
{
    // change to a deque
    std::deque<INPUT_RECORD> inRecords;
    for (size_t i = 0; i < *pcLength; ++i)
    {
        inRecords.push_back(pInputRecord[i]);
    }
    size_t eventsWritten = PrependInputBuffer(inRecords);

    LOG_IF_FAILED(SIZETToDWord(eventsWritten, pcLength));
    return STATUS_SUCCESS;
}

size_t InputBuffer::PrependInputBuffer(_In_ std::deque<INPUT_RECORD>& inRecords)
{
    LOG_IF_FAILED(_HandleConsoleSuspensionEvents(inRecords));
    if (inRecords.empty())
    {
        return STATUS_SUCCESS;
    }
    // read all of the records out of the buffer, then write the
    // prepend ones, then write the original set. We need to do it
    // this way to handle any coalescing that might occur.

    // get all of the existing records, "emptying" the buffer
    std::deque<INPUT_RECORD> existingStorage;
    existingStorage.swap(_storage);

    // write the prepend records
    size_t prependEventsWritten;
    bool setWaitEvent;
    _WriteBuffer(inRecords, prependEventsWritten, setWaitEvent);

    // write all previously existing records
    size_t eventsWritten;
    _WriteBuffer(existingStorage, eventsWritten, setWaitEvent);
    if (setWaitEvent)
    {
        SetEvent(InputWaitEvent);
    }
    WakeUpReadersWaitingForData();
    return prependEventsWritten;
}

// Routine Description:
// - This routine writes to the input buffer.
// Arguments:
// - pInputRecord - Buffer to write from.
// - cInputRecords - On input, number of events to write.
// Return Value:
// Note:
// - The console lock must be held when calling this routine.
DWORD InputBuffer::WriteInputBuffer(_In_ PINPUT_RECORD pInputRecord, _In_ DWORD cInputRecords)
{
    // change to a deque
    std::deque<INPUT_RECORD> inRecords;
    for (size_t i = 0; i < cInputRecords; ++i)
    {
        inRecords.push_back(pInputRecord[i]);
    }
    size_t EventsWritten = WriteInputBuffer(inRecords);
    DWORD result;
    LOG_IF_FAILED(SIZETToDWord(EventsWritten, &result));
    return result;
}

size_t InputBuffer::WriteInputBuffer(_In_ std::deque<INPUT_RECORD>& inRecords)
{
    LOG_IF_FAILED(_HandleConsoleSuspensionEvents(inRecords));
    if (inRecords.empty())
    {
        return 0;
    }

    // Write to buffer.
    size_t EventsWritten;
    bool SetWaitEvent;
    LOG_IF_FAILED(_WriteBuffer(inRecords, EventsWritten, SetWaitEvent));

    if (SetWaitEvent)
    {
        SetEvent(InputWaitEvent);
    }

    // Alert any writers waiting for space.
    WakeUpReadersWaitingForData();
    return EventsWritten;
}

// Routine Description:
// - This routine wakes up readers waiting for data to read.
// Arguments:
// Return Value:
// - TRUE - The operation was successful
// - FALSE/nullptr - The operation failed.
void InputBuffer::WakeUpReadersWaitingForData()
{
    this->WaitQueue.NotifyWaiters(false);
}

// Routine Description:
// - This routine wakes up any readers waiting for data when a ctrl-c or ctrl-break is input.
// Arguments:
// - pInputBuffer - pointer to input buffer
// - Flag - flag indicating whether ctrl-break or ctrl-c was input.
// TODO move to inputBuffer.hpp
void TerminateRead(_Inout_ InputBuffer* pInputBuffer, _In_ WaitTerminationReason Flag)
{
    pInputBuffer->WaitQueue.NotifyWaiters(true, Flag);
}
