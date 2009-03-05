/***********************************************************************
SoundPlayer - Simple class to play sound from a sound file on the local
file system to a playback device. Uses ALSA under Linux, and the Core
Audio frameworks under Mac OS X.
Copyright (c) 2008 Oliver Kreylos

This file is part of the Basic Sound Library (Sound).

The Basic Sound Library is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as published
by the Free Software Foundation; either version 2 of the License, or (at
your option) any later version.

The Basic Sound Library is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License along
with the Basic Sound Library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
***********************************************************************/

#ifdef __DARWIN__
#include <math.h>
#endif
#include <string.h>
#include <Misc/ThrowStdErr.h>
#ifdef __DARWIN__
#include <CoreFoundation/CFURL.h>
#endif

#include <Misc/FileNameExtensions.h>

#include <Sound/SoundPlayer.h>

namespace Sound {

/****************************
Methods of class SoundPlayer:
****************************/

#ifdef __LINUX__

/****************************
Linux version of SoundPlayer:
****************************/

#ifdef SOUND_USE_ALSA

bool SoundPlayer::readWAVHeader(void)
	{
	/* Rewind the file: */
	inputFile.rewind();
	
	/* Read the RIFF chunk: */
	char riffTag[4];
	inputFile.read<char>(riffTag,4);
	if(strncmp(riffTag,"RIFF",4)!=0)
		return false;
	inputFile.read<unsigned int>(); // Skip RIFF chunk size
	char waveTag[4];
	inputFile.read<char>(waveTag,4);
	if(strncmp(waveTag,"WAVE",4)!=0)
		return false;
	
	/* Read the format chunk: */
	char fmtTag[4];
	inputFile.read<char>(fmtTag,4);
	if(strncmp(fmtTag,"fmt ",4)!=0)
		return false;
	size_t fmtChunkSize=inputFile.read<unsigned int>();
	if(fmtChunkSize<2*sizeof(int)+4*sizeof(short int))
		return false;
	if(inputFile.read<unsigned short>()!=1) // Can only do linear PCM samples for now
		return false;
	format.samplesPerFrame=int(inputFile.read<unsigned short>());
	format.framesPerSecond=int(inputFile.read<unsigned int>());
	size_t bytesPerSecond=inputFile.read<unsigned int>();
	size_t bytesPerFrame=inputFile.read<unsigned short>();
	format.bitsPerSample=int(inputFile.read<unsigned short>());
	
	/* Skip any unused data in the format chunk: */
	fmtChunkSize=(fmtChunkSize+1)&~0x1; // Pad to the next two-byte boundary
	if(fmtChunkSize>2*sizeof(int)+4*sizeof(short int))
		inputFile.seekCurrent(fmtChunkSize-(2*sizeof(int)+4*sizeof(short int)));
	
	/* Check if the WAV file's sound data format is compatible and fill in missing data: */
	if(format.bitsPerSample<8||format.bitsPerSample>32||(format.bitsPerSample&0x7)!=0)
		return false;
	if(format.bitsPerSample==24)
		format.bytesPerSample=4; // 24 bit sound data padded into 32 bit words
	else
		format.bytesPerSample=format.bitsPerSample/8;
	format.signedSamples=format.bitsPerSample>8;
	format.sampleEndianness=SoundDataFormat::LittleEndian;
	if(format.samplesPerFrame<1)
		return false;
	if(bytesPerFrame!=size_t(format.samplesPerFrame)*size_t(format.bytesPerSample))
		return false;
	if(bytesPerSecond!=size_t(format.framesPerSecond)*size_t(format.samplesPerFrame)*size_t(format.bytesPerSample))
		return false;
	
	/* Ignore any additional chunks until the data chunk: */
	while(true)
		{
		/* Read the chunk's header: */
		char tag[4];
		if(inputFile.read<char>(tag,4)<4)
			return false;
		unsigned int chunkSize;
		if(inputFile.read<unsigned int>(&chunkSize,1)<1)
			return false;
		
		/* Stop if it's a data chunk: */
		if(strncmp(tag,"data",4)==0)
			break;
		
		/* Skip the chunk: */
		chunkSize=(chunkSize+1)&~0x1; // Pad to two-byte boundary
		inputFile.seekCurrent(chunkSize);
		}
	
	return true;
	}

void* SoundPlayer::playingThreadMethod(void)
	{
	/* Read buffers worth of sound data from the input file until interrupted or at end of file: */
	while(!inputFile.eof())
		{
		/* Read sound data from the input file, up to the buffer size: */
		size_t numBytesRead=inputFile.read(sampleBuffer,sampleBufferSize);
		if(numBytesRead>0)
			{
			/* Write the buffer to the PCM device: */
			snd_pcm_writei(pcmDevice,sampleBuffer,snd_pcm_uframes_t(numBytesRead/bytesPerFrame));
			}
		}
	
	{
	/* Wake up anyone who is waiting for playback to finish: */
	Threads::MutexCond::Lock finishedPlayingLock(finishedPlayingCond);
	active=false;
	finishedPlayingCond.broadcast(finishedPlayingLock);
	}
	
	/* Initiate the get-the-hell-out-of-here maneuver: */
	return 0;
	}

#endif

SoundPlayer::SoundPlayer(const char* inputFileName)
	#ifdef SOUND_USE_ALSA
	:inputFile(inputFileName,"rb",Misc::File::DontCare),
	 bytesPerFrame(0),
	 pcmDevice(0),
	 sampleBufferSize(0),sampleBuffer(0),
	 active(false)
	#else
	:active(false)
	#endif
	{
	#ifdef SOUND_USE_ALSA
	
	/* Determine the input file format from the file name extension: */
	if(Misc::hasCaseExtension(inputFileName,".wav"))
		{
		/* Read the WAV file header: */
		inputFile.setEndianness(Misc::File::LittleEndian);
		if(!readWAVHeader())
			Misc::throwStdErr("SoundPlayer::SoundPlayer: Input file %s is invalid or incompatible WAV file",inputFileName);
		}
	else
		Misc::throwStdErr("SoundPlayer::SoundPlayer: Input file %s has unrecognized extension",inputFileName);
	
	/* Calculate the size of a frame in bytes: */
	bytesPerFrame=size_t(format.samplesPerFrame)*size_t(format.bytesPerSample);
	
	int error;
	
	/* Open the default PCM playback device: */
	error=snd_pcm_open(&pcmDevice,"default",SND_PCM_STREAM_PLAYBACK,0);
	if(error<0)
		Misc::throwStdErr("SoundPlayer::SoundPlayer: Error %s while opening PCM device",snd_strerror(error));
	
	/* Set the PCM device's parameters according to the sound data format: */
	error=format.setPCMDeviceParameters(pcmDevice);
	if(error<0)
		{
		snd_pcm_close(pcmDevice);
		Misc::throwStdErr("SoundPlayer::SoundPlayer: Error %s while setting PCM device parameters",snd_strerror(error));
		}
	
	/* Create a sample buffer holding a quarter second of sound: */
	sampleBufferSize=((size_t(format.framesPerSecond)*250+500)/1000)*bytesPerFrame;
	sampleBuffer=new char[sampleBufferSize];
	
	#endif
	}

SoundPlayer::~SoundPlayer(void)
	{
	#ifdef SOUND_USE_ALSA
	
	{
	/* Stop the playing thread if still active: */
	Threads::MutexCond::Lock finishedPlayingLock(finishedPlayingCond);
	if(active)
		{
		playingThread.cancel();
		playingThread.join();
		
		/* Wake up anybody who is waiting for playback to finish: */
		active=false;
		finishedPlayingCond.broadcast(finishedPlayingLock);
		}
	}
	
	/* Close the PCM device: */
	snd_pcm_close(pcmDevice);
	
	/* Delete the sample buffer: */
	delete[] sampleBuffer;
	
	#endif
	}

SoundDataFormat SoundPlayer::getSoundDataFormat(void) const
	{
	return format;
	}

void SoundPlayer::start(void)
	{
	/* Do nothing if already started: */
	if(active)
		return;
	
	#ifdef SOUND_USE_ALSA
	
	/* Start the background playing thread (which in turn starts the PCM device): */
	active=true;
	playingThread.start(this,&SoundPlayer::playingThreadMethod);
	
	#else
	
	active=true;
	
	#endif
	}

void SoundPlayer::stop(void)
	{
	/* Do nothing if not started: */
	if(!active)
		return;
	
	#ifdef SOUND_USE_ALSA
	
	/* Stop the background playing thread: */
	playingThread.cancel();
	playingThread.join();
	
	{
	/* Wake up anyone who is waiting for playback to finish: */
	Threads::MutexCond::Lock finishedPlayingLock(finishedPlayingCond);
	active=false;
	finishedPlayingCond.broadcast(finishedPlayingLock);
	}
	
	/* Stop the PCM device: */
	snd_pcm_drop(pcmDevice);
	
	#else
	
	active=false;
	
	#endif
	}

#endif

#ifdef __DARWIN__

/*******************************
Mac OS X version of SoundPlayer:
*******************************/

void SoundPlayer::handleOutputBuffer(AudioQueueRef inAQ,AudioQueueBufferRef inBuffer)
	{
	/* Return immediately if playback is stopped: */
	if(!active)
		return;
	
	/* Read the next block of samples from the audio file into the audio buffer: */
	UInt32 numBytesRead;
	UInt32 numPackets=numPacketsPerBuffer;
	AudioFileReadPackets(inputFile,false,&numBytesRead,packetDescriptors,numPlayedPackets,&numPackets,inBuffer->mAudioData);
	
	if(numPackets>0)
		{
		/* Append the just-filled buffer to the audio queue: */
		inBuffer->mAudioDataByteSize=numBytesRead;
		AudioQueueEnqueueBuffer(queue,inBuffer,packetDescriptors!=0?numPackets:0,packetDescriptors);
		
		/* Update the packet counter: */
		numPlayedPackets+=numPackets;
		}
	else
		{
		/* Finish playback: */
		AudioQueueStop(queue,false);
		
		{
		/* Wake up anyone who is waiting for playback to finish: */
		Threads::MutexCond::Lock finishedPlayingLock(finishedPlayingCond);
		active=false;
		finishedPlayingCond.broadcast(finishedPlayingLock);
		}
		}
	}

SoundPlayer::SoundPlayer(const char* inputFileName)
	:bufferSize(0),numPacketsPerBuffer(0),
	 packetDescriptors(0),
	 numPlayedPackets(0),
	 active(false)
	{
	/* Open the given audio file: */
	CFURLRef inputFileURL=CFURLCreateFromFileSystemRepresentation(0,reinterpret_cast<const UInt8*>(inputFileName),strlen(inputFileName),false);
	if(AudioFileOpenURL(inputFileURL,0x1,0,&inputFile)!=noErr)
		{
		CFRelease(inputFileURL);
		Misc::throwStdErr("SoundPlayer::SoundPlayer: Error while opening input file %s",inputFileName);
		}
	CFRelease(inputFileURL);
	
	/* Get the input file's sound data format: */
	UInt32 formatSize=sizeof(format);
	if(AudioFileGetProperty(inputFile,kAudioFilePropertyDataFormat,&formatSize,&format)!=noErr)
		{
		AudioFileClose(inputFile);
		Misc::throwStdErr("SoundPlayer::SoundPlayer: Error while querying sound data format of input file %s",inputFileName);
		}
	
	/* Get an upper bound on the size of packets in the input file to calculate buffer sizes: */
	UInt32 maxPacketSize;
	UInt32 maxPacketSizeSize=sizeof(maxPacketSize);
	if(AudioFileGetProperty(inputFile,kAudioFilePropertyPacketSizeUpperBound,&maxPacketSizeSize,&maxPacketSize)!=noErr)
		{
		AudioFileClose(inputFile);
		Misc::throwStdErr("SoundPlayer::SoundPlayer: Error while querying sound data packet size of input file %s",inputFileName);
		}
	
	/* Create a playback audio queue: */
	if(AudioQueueNewOutput(&format,handleOutputBufferWrapper,this,0,kCFRunLoopCommonModes,0,&queue)!=noErr)
		{
		AudioFileClose(inputFile);
		Misc::throwStdErr("SoundPlayer::SoundPlayer: Error while creating playback audio queue");
		}
	
	/* Calculate an appropriate audio buffer size: */
	if(format.mFramesPerPacket!=0)
		{
		numPacketsPerBuffer=UInt32(floor(format.mSampleRate*0.25+0.5));
		bufferSize=numPacketsPerBuffer*maxPacketSize;
		}
	else
		{
		bufferSize=maxPacketSize>0x10000U?maxPacketSize:0x10000U;
		numPacketsPerBuffer=bufferSize/maxPacketSize;
		}
	
	/* Copy any magic cookies from the input file to the audio queue: */
	UInt32 magicCookieSize=sizeof(UInt32); // Estimated size, to get things rolling
	if(AudioFileGetPropertyInfo(inputFile,kAudioFilePropertyMagicCookieData,&magicCookieSize,0)==noErr&&magicCookieSize>0)
		{
		/* Get the magic cookie from the audio file: */
		char* magicCookie=new char[magicCookieSize];
		if(AudioFileGetPropertyInfo(inputFile,kAudioFilePropertyMagicCookieData,&magicCookieSize,reinterpret_cast<UInt32*>(magicCookie))!=noErr)
			{
			/* This really shouldn't have happened! */
			delete[] magicCookie;
			AudioQueueDispose(queue,true);
			AudioFileClose(inputFile);
			Misc::throwStdErr("SoundPlayer::SoundPlayer: Error while setting the audio queue's magic cookie");
			}
		
		/* Set the magic cookie in the audio queue: */
		if(AudioQueueSetProperty(queue,kAudioQueueProperty_MagicCookie,magicCookie,magicCookieSize)!=noErr)
			{
			delete[] magicCookie;
			AudioQueueDispose(queue,true);
			AudioFileClose(inputFile);
			Misc::throwStdErr("SoundPlayer::SoundPlayer: Error while setting the audio queue's magic cookie");
			}
		delete[] magicCookie;
		}
	
	/* Allocate the audio playback buffers: */
	for(int i=0;i<2;++i)
		{
		if(AudioQueueAllocateBuffer(queue,bufferSize,&buffers[i])!=noErr)
			{
			AudioQueueDispose(queue,true);
			AudioFileClose(inputFile);
			Misc::throwStdErr("SoundPlayer::SoundPlayer: Error while allocating audio buffer %d",i);
			}
		}
	
	/* Check if the input file's format is variable bit rate: */
	if(format.mBytesPerPacket==0||format.mFramesPerPacket==0)
		{
		/* Create the array of packet descriptors: */
		packetDescriptors=new AudioStreamPacketDescription[numPacketsPerBuffer];
		}
	
	/* Set the audio queue's playback gain: */
	AudioQueueSetParameter(queue,kAudioQueueParam_Volume,1.0); // Don't care if this fails
	}

SoundPlayer::~SoundPlayer(void)
	{
	{
	/* Stop playback if still active: */
	Threads::MutexCond::Lock finishedPlayingLock(finishedPlayingCond);
	if(active)
		{
		AudioQueueStop(queue,true);
		
		/* Wake up anybody who is waiting for playback to finish: */
		active=false;
		finishedPlayingCond.broadcast(finishedPlayingLock);
		}
	}
	
	/* Destroy the packet descriptors: */
	delete[] packetDescriptors;
	
	/* Destroy the audio queue: */
	AudioQueueDispose(queue,true);
	
	/* Close the audio file: */
	AudioFileClose(inputFile);
	}

SoundDataFormat SoundPlayer::getSoundDataFormat(void) const
	{
	/* Extract relevant information from the format data structure: */
	SoundDataFormat result;
	result.bitsPerSample=format.mBitsPerChannel;
	result.bytesPerSample=format.mBitsPerChannel/8;
	result.signedSamples=(format.mFormatFlags&kLinearPCMFormatFlagIsSignedInteger)!=0;
	result.sampleEndianness=format.mFormatFlags&kLinearPCMFormatFlagIsBigEndian?SoundDataFormat::BigEndian:SoundDataFormat::LittleEndian;
	result.samplesPerFrame=format.mChannelsPerFrame;
	result.framesPerSecond=int(floor(format.mSampleRate+0.5));
	
	return result;
	}

void SoundPlayer::start(void)
	{
	/* Do nothing if already started: */
	if(active)
		return;
	
	/* Rewind the audio file: */
	numPlayedPackets=0;
	
	/* Prime the audio queue by reading the first set of buffers from the input file: */
	for(int i=0;i<2;++i)
		{
		/* Read the next block of samples from the audio file into the audio buffer: */
		UInt32 numBytesRead;
		UInt32 numPackets=numPacketsPerBuffer;
		AudioFileReadPackets(inputFile,false,&numBytesRead,packetDescriptors,numPlayedPackets,&numPackets,buffers[i]->mAudioData);
		
		if(numPackets>0)
			{
			/* Append the just-filled buffer to the audio queue: */
			buffers[i]->mAudioDataByteSize=numBytesRead;
			AudioQueueEnqueueBuffer(queue,buffers[i],packetDescriptors!=0?numPackets:0,packetDescriptors);
			
			/* Update the packet counter: */
			numPlayedPackets+=numPackets;
			}
		}
	AudioQueuePrime(queue,0,0);
	
	/* Start the audio queue: */
	active=true;
	if(AudioQueueStart(queue,0)!=noErr)
		{
		active=false;
		Misc::throwStdErr("SoundPlayer::start: Error while starting audio queue");
		}
	}

void SoundPlayer::stop(void)
	{
	/* Do nothing if not started: */
	if(!active)
		return;
	
	/* Stop the audio queue: */
	AudioQueueStop(queue,true);
	
	{
	/* Wake up anyone who is waiting for playback to finish: */
	Threads::MutexCond::Lock finishedPlayingLock(finishedPlayingCond);
	active=false;
	finishedPlayingCond.broadcast(finishedPlayingLock);
	}
	}

#endif

void SoundPlayer::wait(void)
	{
	Threads::MutexCond::Lock finishedPlayingLock(finishedPlayingCond);
	
	/* Do nothing if not started: */
	if(!active)
		return;
	
	/* Block on the condition variable: */
	finishedPlayingCond.wait(finishedPlayingLock);
	}

}
