"""
STK500v2 protocol implementation for programming AVR chips.
The STK500v2 protocol is used by the ArduinoMega2560 and a few other Arduino platforms to load firmware.
"""
__copyright__ = "Copyright (C) 2013 David Braam - Released under terms of the AGPLv3 License"
import os, struct, sys, time

from serial import Serial
from serial import SerialException
from serial import SerialTimeoutException

import ispBase, intelHex

class Stk500v2(ispBase.IspBase):
	def __init__(self):
		self.serial = None
		self.seq = 1
		self.lastAddr = -1
		self.progressCallback = None

	def connect(self, port = 'COM22', speed = 115200):
		if self.serial is not None:
			self.close()
		try:
			self.serial = Serial(str(port), speed, timeout=1, writeTimeout=10000)
		except SerialException as e:
			raise ispBase.IspError("Failed to open serial port")
		except:
			raise ispBase.IspError("Unexpected error while connecting to serial port:" + port + ":" + str(sys.exc_info()[0]))
		self.seq = 1

		#Reset the controller
		for n in xrange(0, 2):
			self.serial.setDTR(True)
			time.sleep(0.1)
			self.serial.setDTR(False)
			time.sleep(0.1)
		time.sleep(0.2)

		self.serial.flushInput()
		self.serial.flushOutput()
		self.sendMessage([1])
		if self.sendMessage([0x10, 0xc8, 0x64, 0x19, 0x20, 0x00, 0x53, 0x03, 0xac, 0x53, 0x00, 0x00]) != [0x10, 0x00]:
			self.close()
			raise ispBase.IspError("Failed to enter programming mode")

		self.sendMessage([0x06, 0x80, 0x00, 0x00, 0x00])
		if self.sendMessage([0xEE])[1] == 0x00:
			self._has_checksum = True
		else:
			self._has_checksum = False
		self.serial.timeout = 5

	def close(self):
		if self.serial is not None:
			self.serial.close()
			self.serial = None

	#Leave ISP does not reset the serial port, only resets the device, and returns the serial port after disconnecting it from the programming interface.
	#	This allows you to use the serial port without opening it again.
	def leaveISP(self):
		if self.serial is not None:
			if self.sendMessage([0x11]) != [0x11, 0x00]:
				raise ispBase.IspError("Failed to leave programming mode")
			ret = self.serial
			self.serial = None
			return ret
		return None

	def isConnected(self):
		return self.serial is not None

	def hasChecksumFunction(self):
		return self._has_checksum

	def sendISP(self, data):
		recv = self.sendMessage([0x1D, 4, 4, 0, data[0], data[1], data[2], data[3]])
		return recv[2:6]

	def writeFlash(self, flashData):
		#Set load addr to 0, in case we have more then 64k flash we need to enable the address extension
		pageSize = self.chip['pageSize'] * 2
		flashSize = pageSize * self.chip['pageCount']

		# pad the data up to page boundary
		while (len(flashData) % pageSize) != 0:
			flashData.append(0xFF)

		if flashSize > 0xFFFF:
			self.sendMessage([0x06, 0x80, 0x00, 0x00, 0x00])
		else:
			self.sendMessage([0x06, 0x00, 0x00, 0x00, 0x00])

		loadCount = (len(flashData) + pageSize - 1) / pageSize
		for i in xrange(0, loadCount):
			# Frank26080115: we need to place code in front of the STK500v2 bootloader, and also behind it, but not on top of it
			# So skip the bootloader region that we don't care about
			if (i * pageSize) >= (flashSize - (1024 * 8)) and i < (loadCount - 1):
				continue
			# If we skipped stuff, then we need to set the address again
			if i >= (loadCount - 1):
				j = i * pageSize / 2
				if flashSize > 0xFFFF:
					self.sendMessage([0x06, 0x80 | (((j & 0xFF000000) >> (8 * 3)) & 0x7F), (((j & 0xFF0000) >> (8 * 2)) & 0xFF), (((j & 0xFF00) >> 8) & 0xFF), (j & 0xFF)])
				else:
					self.sendMessage([0x06, 0x00, 0x00, (((j & 0xFF00) >> 8) & 0xFF), (j & 0xFF)])
			recv = self.sendMessage([0x13, pageSize >> 8, pageSize & 0xFF, 0xc1, 0x0a, 0x40, 0x4c, 0x20, 0x00, 0x00] + flashData[(i * pageSize):(i * pageSize + pageSize)])
			if self.progressCallback is not None:
				if self._has_checksum:
					self.progressCallback(i + 1, loadCount)
				else:
					self.progressCallback(i + 1, loadCount*2)

	def verifyFlash(self, flashData):
		if self._has_checksum and len(flashData) < ((256 - 8) * 0x100):
			self.sendMessage([0x06, 0x00, (len(flashData) >> 17) & 0xFF, (len(flashData) >> 9) & 0xFF, (len(flashData) >> 1) & 0xFF])
			res = self.sendMessage([0xEE])
			checksum_recv = res[2] | (res[3] << 8)
			checksum = 0
			for d in flashData:
				checksum += d
			checksum &= 0xFFFF
			if hex(checksum) != hex(checksum_recv):
				raise ispBase.IspError('Verify checksum mismatch: 0x%x != 0x%x' % (checksum & 0xFFFF, checksum_recv))
		else:
			#Set load addr to 0, in case we have more then 64k flash we need to enable the address extension
			flashSize = self.chip['pageSize'] * 2 * self.chip['pageCount']
			if flashSize > 0xFFFF:
				self.sendMessage([0x06, 0x80, 0x00, 0x00, 0x00])
			else:
				self.sendMessage([0x06, 0x00, 0x00, 0x00, 0x00])

			loadCount = (len(flashData) + 0xFF) / 0x100
			for i in xrange(0, loadCount):
				k = i * 0x100
				j = k / 2
				# skip the bootloader region that we don't care about
				if k >= (flashSize - (1024 * 8)) and i < (loadCount - 1):
					continue
				if i >= (loadCount - 1):
					if flashSize > 0xFFFF:
						self.sendMessage([0x06, 0x80 | (((j & 0xFF000000) >> (8 * 3)) & 0x7F), (((j & 0xFF0000) >> (8 * 2)) & 0xFF), (((j & 0xFF00) >> 8) & 0xFF), (j & 0xFF)])
					else:
						self.sendMessage([0x06, 0x00, 0x00, (((j & 0xFF00) >> 8) & 0xFF), (j & 0xFF)])
				recv = self.sendMessage([0x14, 0x01, 0x00, 0x20])[2:0x102]
				if self.progressCallback is not None:
					self.progressCallback(loadCount + i + 1, loadCount*2)
				for j in xrange(0, 0x100):
					if i * 0x100 + j < len(flashData) and flashData[i * 0x100 + j] != recv[j]:
						if i < (loadCount - 1):
							raise ispBase.IspError('Verification Error at: 0x%X' % (i * 0x100 + j))
						else:
							raise ispBase.IspError('Backdoor Code Injection Failed. Verification Error at: 0x%X' % (i * 0x100 + j))

	def sendMessage(self, data):
		message = struct.pack(">BBHB", 0x1B, self.seq, len(data), 0x0E)
		for c in data:
			message += struct.pack(">B", c)
		checksum = 0
		for c in message:
			checksum ^= ord(c)
		message += struct.pack(">B", checksum)
		try:
			self.serial.write(message)
			self.serial.flush()
		except SerialTimeoutException:
			raise ispBase.IspError('Serial send timeout')
		self.seq = (self.seq + 1) & 0xFF
		return self.recvMessage()

	def recvMessage(self):
		state = 'Start'
		checksum = 0
		while True:
			s = self.serial.read()
			if len(s) < 1:
				raise ispBase.IspError("Timeout")
			b = struct.unpack(">B", s)[0]
			checksum ^= b
			#print(hex(b))
			if state == 'Start':
				if b == 0x1B:
					state = 'GetSeq'
					checksum = 0x1B
			elif state == 'GetSeq':
				state = 'MsgSize1'
			elif state == 'MsgSize1':
				msgSize = b << 8
				state = 'MsgSize2'
			elif state == 'MsgSize2':
				msgSize |= b
				state = 'Token'
			elif state == 'Token':
				if b != 0x0E:
					state = 'Start'
				else:
					state = 'Data'
					data = []
			elif state == 'Data':
				data.append(b)
				if len(data) == msgSize:
					state = 'Checksum'
			elif state == 'Checksum':
				if checksum != 0:
					state = 'Start'
				else:
					return data

def portList():
	ret = []
	import _winreg
	key=_winreg.OpenKey(_winreg.HKEY_LOCAL_MACHINE,"HARDWARE\\DEVICEMAP\\SERIALCOMM")
	i=0
	while True:
		try:
			values = _winreg.EnumValue(key, i)
		except:
			return ret
		if 'USBSER' in values[0]:
			ret.append(values[1])
		i+=1
	return ret

def runProgrammer(port, filename):
	""" Run an STK500v2 program on serial port 'port' and write 'filename' into flash. """
	programmer = Stk500v2()
	programmer.connect(port = port)
	programmer.programChip(intelHex.readHex(filename))
	programmer.close()

def main():
	""" Entry point to call the stk500v2 programmer from the commandline. """
	import threading
	if sys.argv[1] == 'AUTO':
		print portList()
		for port in portList():
			threading.Thread(target=runProgrammer, args=(port,sys.argv[2])).start()
			time.sleep(5)
	else:
		programmer = Stk500v2()
		programmer.connect(port = sys.argv[1])
		programmer.programChip(intelHex.readHex(sys.argv[2]))
		sys.exit(1)

if __name__ == '__main__':
	main()
