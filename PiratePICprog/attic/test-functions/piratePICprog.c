/*
 
 Pirate-Loader for Bootloader v4
 
 Version  : 1.0.2
 
 Changelog:
 
  + 2010-02-04 - Changed polling interval to 10ms on Windows select wrapper, suggested by Michal (robots)
  
  + 2010-02-04 - Added sleep(0) between write instructions, patch submitted by kbulgrien
 
  + 2010-01-22 - Added loader version number to the console output and source code
 
  + 2010-01-19 - Fixed BigEndian incompatibility
			   - Added programming simulate switch ( --simulate ) for data verification 
 
  + 2010-01-18 - Initial release
 
 
 Building:
 
  UNIX family systems:
	
	gcc pirate-loader.c -o pirate-loader
 
  WINDOWS:
    
	cl pirate-loader.c /DWIN32=1
 
 
 Usage:
	
	Run ./pirate-loader --help for more information on usage and possible switches
 
 */

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <fcntl.h>
#include <errno.h>

#include "buspirateio.h"
#include "serial.h"

#define PIRATE_LOADER_VERSION "1.0.2"

#define STR_EXPAND(tok) #tok
#define OS_NAME(tok) STR_EXPAND(tok)


#ifdef WIN32
	#include <windows.h>
	#include <time.h>

	#define O_NOCTTY 0
	#define O_NDELAY 0
	#define B115200 115200

	#define OS WINDOWS
#else
	#include <unistd.h>
	#include <termios.h>
	#include <sys/select.h>
	#include <sys/types.h>
	#include <sys/time.h>
#endif

/* macro definitions */

#if !defined OS
#define OS UNKNOWN
#endif

#define PIC_FLASHSIZE 0xAC00

#define PIC_NUM_PAGES 256
#define PIC_NUM_ROWS_IN_PAGE  1
#define PIC_NUM_WORDS_IN_ROW 32

#define PIC_WORD_SIZE  (2)
#define PIC_ROW_SIZE  (PIC_NUM_WORDS_IN_ROW * PIC_WORD_SIZE)
#define PIC_PAGE_SIZE (PIC_NUM_ROWS_IN_PAGE  * PIC_ROW_SIZE)


#define PIC_ROW_ADDR(p,r)		(((p) * PIC_PAGE_SIZE) + ((r) * PIC_ROW_SIZE))
#define PIC_WORD_ADDR(p,r,w)	(PIC_ROW_ADDR(p,r) + ((w) * PIC_WORD_SIZE))
#define PIC_PAGE_ADDR(p)		(PIC_PAGE_SIZE * (p))

/* type definitions */

typedef unsigned char  uint8;
typedef unsigned short uint16;
typedef unsigned long  uint32;

void PIC18_write(uint32 tblptr, uint8* Data, int length);

/* global settings, command line arguments */

uint8		g_verbose = 0;
uint8		g_hello_only = 0;
uint8		g_simulate = 0;
const char* g_device_path  = NULL;
const char* g_hexfile_path = NULL;
int		dev_fd = -1;

/* non-firmware functions */
int parseCommandLine(int argc, const char** argv)
{
	int i = 0;
	
	for(i=1; i<argc; i++)
	{
		
		if( !strncmp(argv[i], "--hex=", 6) ) {
			g_hexfile_path = argv[i] + 6;
		} else if ( !strncmp(argv[i], "--dev=", 6) ) {
			g_device_path = argv[i] + 6;
		} else if ( !strcmp(argv[i], "--verbose") ) {
			g_verbose = 1;
		} else if ( !strcmp(argv[i], "--hello") ) {
			g_hello_only = 1;
		} else if ( !strcmp(argv[i], "--simulate") ) {
			g_simulate = 1;
		} else if ( !strcmp(argv[i], "--help") ) {
			argc = 1; //that's not pretty, but it works :)
			break;
		} else {
			fprintf(stderr, "Unknown parameter %s, please use pirate-loader --help for usage\n", argv[i]);
			return -1;
		}
	}
	
	if( argc == 1 )
	{
		//print usage
		puts("pirate-loader usage:\n");
		puts(" ./pirate-loader --dev=/path/to/device --hello");
		puts(" ./pirate-loader --dev=/path/to/device --hex=/path/to/hexfile.hex [ --verbose");
		puts(" ./pirate-loader --simulate --hex=/path/to/hexfile.hex [ --verbose");
		puts("");
		
		return 0;
	}
	
	return 1;
}

/* Send an array to the serail port , maybe should be moved to serail.c */
int sendString(int fd, int length, uint8* dat)
{
	int res=0;
	
	res = write(fd, dat, length);
	
	if( res <= 0 ) {
		puts("ERROR");
		return -1;
	}
}

//
/* Bus Pirate BINMODE related functions */
//

//low lever send command, get reply function
int writetopirate(uint8* val){
	int res = -1;
	uint8	buffer[2] = {0};
	
	sendString(dev_fd, 1, val);
	res = readWithTimeout(dev_fd, buffer, 1, 1);
	if( memcmp(buffer, "\x01", 1) ) {
		puts("ERROR");
		return -1;
	} 
	return 0;
}

//binmode: bulk write bytes to bus command
int BulkByteWrite(int bwrite, uint8* val){
	int i;
	uint8 opcode=0x10;
	opcode=opcode|(bwrite-1);

	writetopirate(&opcode);	
	for(i=0;i<bwrite;i++){
		writetopirate(&val[i]);	
	}
}

//
/* HEX file related functions */
//
unsigned char hexdec(const char* pc)
{
	return (((pc[0] >= 'A') ? ( pc[0] - 'A' + 10 ) : ( pc[0] - '0' ) ) << 4 | 
			((pc[1] >= 'A') ? ( pc[1] - 'A' + 10 ) : ( pc[1] - '0' ) )) & 0x0FF;
	
}

void dumpHex(uint8* buf, uint32 len)
{
	uint32 i=0;
	
	for(i=0; i<len; i++){
		printf("%02X ", buf[i]);
	}
	putchar('\n');
}

int readHEX(const char* file, uint8* bout, unsigned long max_length, uint8* pages_used)
{
	static const uint32 HEX_DATA_OFFSET = 4;
	uint8  linebin[256] = {0};
	uint8* data = (linebin + HEX_DATA_OFFSET);
	uint8  hex_crc, hex_type, hex_len;
	uint32 hex_addr;
	uint32 hex_base_addr = 0;
	uint32 hex_words     = 0;
	
	uint32 f_addr = 0;
	uint32 o_addr = 0;
	
	uint32 num_words = 0;
	
	char  line[512] = {0};
	char  *pc;
	char  *pline = line + 1; //location in the line of the start of the length????
	int   res = 0;
	int	  binlen = 0;
	int   line_no = 0;
	int   i = 0;
	
	FILE* fp = fopen(file, "rb");
	
	if( !fp ) {
		return -1;
	}
	
	while( !feof(fp) && fgets(line, sizeof(line) - 1, fp) )
	{
		line_no++;//increment line counter

		if( line[0] != ':' ) {//if the line doesn't start with : then the HEX is mangled somehow
			break;
		}
		
		res = strlen(pline); 
		pc  = pline + res - 1;
		
		while( pc > pline && *pc <= ' ' ) { //???get address or length????
			*pc-- = 0;
			res--;
		}
		
		if( res & 0x01 || res > 512 || res < 10) {
			fprintf(stderr, "Incorrect number of characters on line %d:%d\n", line_no, res);
			return -1;
		}
		
		hex_crc = 0;
		
		for( pc = pline, i = 0; i<res; i+=2, pc+=2 ) {
			linebin[i >> 1] = hexdec(pc);//crawl line two bytes at a time and suck up the HEX digits, put in linebin[i/2]
			hex_crc += linebin[i >> 1];//handle CRC
		}
		
		binlen = res / 2;
		
		if( hex_crc != 0 ) { //each byte plus the checksum (final hex number) is 0
			fprintf(stderr, "Checksum does not match, line %d\n", line_no);
			return -1;
		}
		
		hex_addr = (linebin[1] << 8) | linebin[2]; //this line address in program space
		hex_len  = linebin[0]; //length of the data
		hex_type = linebin[3]; //HEX record type
		
		if( binlen - (1 + 2 + 1 + hex_len + 1) != 0 ) {
			fprintf(stderr, "Incorrect number of bytes, line %d\n", line_no);
			return -1;
		}
		
		if( hex_type == 0x00 )
		{
			f_addr  = (hex_base_addr | (hex_addr)) / 2; //PCU

			if( hex_len % 4 ) {
				fprintf(stderr, "Misaligned data, line %d\n", line_no);
				return -1;
			} else if( f_addr >= PIC_FLASHSIZE ) {
				fprintf(stderr, "Current record address is higher than maximum allowed, line %d\n", line_no);
				return -1;
			}
			
			hex_words = hex_len  / 2; //not 4...
			o_addr  = (f_addr) * PIC_WORD_SIZE; //BYTES
			
			for( i=0; i<hex_words; i++)
			{
				bout[o_addr + 0] = data[(i*2) + 0];//2 not 4
				bout[o_addr + 1] = data[(i*2) + 1];//2 not 4
				//bout[o_addr + 2] = data[(i*4) + 1];
//				printf("Fad: %d Oad: %d HW: %d W1: %X W2: %X \n", f_addr, o_addr,hex_words,bout[o_addr + 0],bout[o_addr + 1] );
				pages_used[ (o_addr / PIC_PAGE_SIZE) ] = 1;
				
				o_addr    += PIC_WORD_SIZE;
				num_words ++;
			}

		} else if ( hex_type == 0x04 && hex_len == 2) {
			hex_base_addr = (linebin[4] << 24) | (linebin[5] << 16);
		} else if ( hex_type == 0x01 ) {
			break; //EOF
		} else {
			fprintf(stderr, "Unsupported record type %02x, line %d\n", hex_type, line_no);
			return -1;
		}
		
	}
	
	fclose(fp);
	//puts("Raw HEX...");
	//dumpHex(bout,64);
	return num_words;
}

//
// PIC 16/18/24 functions
//

int PIC18_sendFirmware(int fd, uint8* data, uint8* pages_used)
{
	uint32 u_addr;
	
	
	uint32 page  = 0;
	uint32 done  = 0;
	uint32 row   = 0;
	uint8  command[256] = {0};
	
	
	//for( page=0; page<PIC_NUM_PAGES; page++)
	//for( page=0; page<3; page++)
	//{
		
		u_addr = page * ( PIC_NUM_WORDS_IN_ROW * 2 * PIC_NUM_ROWS_IN_PAGE );
		//u_addr = page * ( 2 * 32 );
		
		if( pages_used[page] != 1 ) {
			if( g_verbose && u_addr < PIC_FLASHSIZE) {
				fprintf(stdout, "Skipping page %ld [ %06lx ], not used\n", page, u_addr);
			}
			//continue;
		}
		
		if( u_addr >= PIC_FLASHSIZE ) {
			fprintf(stderr, "Address out of flash\n");
			//continue; //return -1;
		}
		
		//write 64 bytes
		//for( row = 0; row < PIC_NUM_ROWS_IN_PAGE; row ++, u_addr += (PIC_NUM_WORDS_IN_ROW * 2))
		//{
		
			memcpy(&command[0], &data[page*64], 64);
			
			printf("Writing page %ld, %04lx... \n", page, u_addr);
			if( g_verbose ) {
				dumpHex(command,64);
			}

			PIC18_write(u_addr, command, 64);
			
			done += 64;//PIC_ROW_SIZE;
		//}
	//}
	
	return done;
}

unsigned char BP_reversebyte(unsigned char c){
        unsigned char r, i;

        for(i=0x01 ; i!=0; i=i<<1){
            r=r<<1;
            if(c&i)r|=0x01;
        }
        return r;
}


int PIC416Write(uint8 cmd, uint16 data){
	uint8 buffer[4]={0};
	int res = -1;
	
	buffer[0]='\xA4';
	buffer[1]=cmd;
	buffer[2]=BP_reversebyte((uint8)(data));
	buffer[3]=BP_reversebyte((data>>8));

	
	sendString(dev_fd, 4, buffer);
	res = readWithTimeout(dev_fd, buffer, 1, 1);
	if( memcmp(buffer, "\x01", 1) ) {
		puts("ERROR");
		return -1;
	} 
	return 0;
}

uint8 PIC416Read(uint8 cmd){
	uint16 PICread;
	uint8 buffer[2]={0};
	int res = -1;
	
	
	buffer[0]='\xA5';
	buffer[1]=cmd;

	sendString(dev_fd, 2, buffer);
	res = readWithTimeout(dev_fd, buffer, 1, 1);
	PICread=BP_reversebyte(buffer[0]);
	
	return PICread;
}

uint16 PIC424Read(void){
	uint8 buffer[4]={0};
	int res = -1;
	uint16 r;
	
	sendString(dev_fd, 1, "\xA5");
	res = readWithTimeout(dev_fd, buffer, 2, 1);
	
	//read device ID, two bytes takes 2 read operations, each gets a byte
	r=buffer[0];	//lower 8 bits
	r|=buffer[1]<<8;	//upper 8 bits
	return r;
}

int PIC424Write(uint32 data, uint8 prenop, uint8 postnop){
	uint8 buffer[5]={0};
	int res = -1;
	
	buffer[0]='\xA4';
	buffer[1]=(uint8)(data);
	buffer[2]=(data>>8);
	buffer[3]=(data>>16);
	buffer[4]=((prenop<<4)|(postnop&0x0F));

	
	sendString(dev_fd, 5, buffer);
	res = readWithTimeout(dev_fd, buffer, 1, 1);
	if( memcmp(buffer, "\x01", 1) ) {
		puts("ERROR");
		return -1;
	} 
	return 0;
}
#define PIC24NOP() 	PIC424Write(0x000000,0,0)

void DataLow(){
	writetopirate("\x0C");
}
void DataHigh(){
	writetopirate("\x0D");
}
void ClockLow(){
	writetopirate("\x0A");
}
void ClockHigh(){
	writetopirate("\x0B");
}
void MCLRLow(){
	writetopirate("\x04");
}
void MCLRHigh(){
	writetopirate("\x05");
}

//
/* Common programming functions */
//
void exitICSP(void){
	//exit programming mode
	MCLRLow();
	MCLRHigh();
}

//enter ICSP on PICs with low voltage ICSP mode
//pulls MCLR low, sends programming mode entry key, releases MCLR
void PIC18_enterLowVPPICSP(uint32 icspkey){
	uint8	buffer[4];
	
	//all programming operations are LSB first, but the ICSP entry key is MSB first. 
	// Reconfigure the mode for LSB order
	//printf("Set mode for MCLR (MSB)...");
	if(writetopirate("\x88")){
		puts("Set mode for MCLR (MSB)...ERROR");
		//goto Error;
	} 
	//puts("(OK)");
	
	ClockLow();
    DataLow();
    MCLRLow();
    MCLRHigh();
    MCLRLow();
   //send ICSP key 0x4D434850
	buffer[0]=icspkey>>24;
	buffer[1]=icspkey>>16;
	buffer[2]=icspkey>>8;
	buffer[3]=icspkey;	
    BulkByteWrite(4,buffer);
    DataLow();
    MCLRHigh();
	
	//all programming operations are LSB first, but the ICSP entry key is MSB first. 
	// Reconfigure the mode for LSB order
	//printf("Set mode for PIC programming (LSB)...");
	if(writetopirate("\x8A")){
		puts("Set mode for PIC programming (LSB)...ERROR");
		//goto Error;
	} 
	//puts("(OK)");
}

//this is aboiut the same as the 18FxxJxx chip, except it also does the first 9bit SIX command
void PIC24_enterLowVPPICSP(uint32 icspkey){
	uint8	buffer[4];
	
	//all programming operations are LSB first, but the ICSP entry key is MSB first. 
	// Reconfigure the mode for LSB order
	//printf("Set mode for MCLR (MSB)...");
	if(writetopirate("\x88")){
		puts("Set mode for MCLR (MSB)...ERROR");
		//goto Error;
	} 
	//puts("(OK)");
	
	ClockLow();
    DataLow();
    MCLRLow();
    MCLRHigh();
    MCLRLow();
   //send ICSP key 0x4D434850
	buffer[0]=icspkey>>24;
	buffer[1]=icspkey>>16;
	buffer[2]=icspkey>>8;
	buffer[3]=icspkey;	
    BulkByteWrite(4,buffer);
    DataLow();
    MCLRHigh();
	
	//all programming operations are LSB first, but the ICSP entry key is MSB first. 
	// Reconfigure the mode for LSB order
	//printf("Set mode for PIC programming (LSB)...");
	if(writetopirate("\x8A")){
		puts("Set mode for PIC programming (LSB)...ERROR");
		//goto Error;
	} 
	//puts("(OK)");

	//first 9bit SIX command, setup the PIC ICSP
	writetopirate("\x34");//five extra clocks on first SIX command after ICSP mode
	writetopirate("\x00");//all 0
	PIC424Write(0x040200,0,0);
	PIC424Write(0x040200,0,3);//SIX,0x040200,5, NOP
	PIC424Write(0x040200,0,1);//SIX,0x040200,5, goto 0x200
}

//enable 13volts using high voltage programming adapter
void enterHighVPPICSP(void){}

//
/* 18F2/4xJxx related functions, maybe other 18Fs too? */
//

void PIC18_settblptr(uint32 tblptr)
{
	// set TBLPTR
	PIC416Write(0x00,0x0E00 | ((tblptr>>16)&0xff));
	PIC416Write(0x00,0x6EF8);
	PIC416Write(0x00,0x0E00 | ((tblptr>>8)&0xff));
	PIC416Write(0x00,0x6EF7);
	PIC416Write(0x00,0x0E00 | ((tblptr)&0xff));
	PIC416Write(0x00,0x6EF6);
}
//should probably be PIC18FreadID(uint32 DEVIDlocation), with the location of the ID as a passed variable
uint16 PIC18_readID(uint32 tblptr){
	uint16 PICid;
	//0x3ffffe
	//setup read from device ID bits
	PIC18_settblptr(tblptr);
	
	//read device ID, two bytes takes 2 read operations, each gets a byte
	PICid=PIC416Read(0x09);	//lower 8 bits
	PICid|=PIC416Read(0x09)<<8;	//upper 8 bits
	return PICid;
}

void PIC24_read(uint32 addr, uint16* Data, int length){
	int ctr, nopctr=0;
	
	PIC424Write(0x200000|((addr&0xffff0000)>>12), 0,0);//SIX,0x200FF0,5, N/A MOV #<SourceAddress23:16>, W0
	PIC424Write(0x880190,0,0);//SIX,0x880190,5, N/AMOV W0, TBLPAG
	PIC424Write(0x200006|((addr&0x000ffff)<<4), 0,2 );//SIX,0x200006,5, N/A MOV #<SourceAddress15:0>, W6
	
	PIC424Write(0x207847,0,1);//SIX,0x200007,5, N/A  //MOV #VISI,W7
	//PIC24NOP();//SIX,0x000000,5, N/A
	
	for(ctr=0; ctr<length; ctr++){
		PIC424Write(0xBA0BB6,0,2); //SIX,0xBA0BB6,5, N/A TBLRDH.B [W6++], [W7++]
		Data[ctr]=PIC424Read(); //REGOUT,0x000000,5, read VISI register (PIC includes 2 NOPS after every read, may need to be updated later)
		//printf("Read: %X \n",Data[ctr]); //REGOUT,0x000000,5, read VISI register
	
		//every so often we need to reset the address pointer or it will fall off the end
		//also do it the last time
		nopctr++;
		if((nopctr>10)||((ctr+1)==length)){
			nopctr=0; //only do occasionally
			PIC24NOP();//SIX,0x000000,5, N/A
			PIC424Write(0x040200,0,2);//SIX,0xBA0BB6,5, N/A TBLRDH.B [W6++], [W7++] (this needs a pre-NOP)
		}
	}
}



//should probably be PIC18FreadID(uint32 DEVIDlocation), with the location of the ID as a passed variable
void PIC18_read(uint32 tblptr, uint8* Data, int length)
{
	int ctr;
	//setup read 
	PIC18_settblptr(tblptr);
	//read device
	for(ctr=0;ctr<length;ctr++)
	{
		//printf("%X ", PIC416Read(0x09) );
		Data[ctr]=PIC416Read(0x09);
	}
}
//a few things need to be done once at the beginning of a sequence of write operations
//this configures the PIC, and enables page writes
//call it once, then call PIC18F_write() as needed
void PIC18_setupwrite(void){
	PIC416Write(0x00,0x8EA6); //setup PIC
	PIC416Write(0x00,0x9CA6); //setup PIC
	PIC416Write(0x00,0x84A6); //enable page writes
}
//18F setup write location and write length bytes of data to PIC
void PIC18_write(uint32 tblptr, uint8* Data, int length)
{
	uint16 DataByte;//, buffer[2]={0x00,0x00};
	int ctr;
	uint8	buffer[4] = {0};

	// set TBLPTR
	PIC18_settblptr(tblptr); 

	//PIC416Write(0x00,0x6AA6); //doesn't seem to be needed now
	//PIC416Write(0x00,0x88A6);

	for(ctr=0;ctr<length-2;ctr+=2)
		{
		DataByte=Data[ctr+1];
		DataByte=DataByte<<8;
		DataByte|=Data[ctr];
		PIC416Write(0x0D,DataByte);
		}
	DataByte=Data[length-1];
	DataByte=DataByte<<8;
	DataByte|=Data[length-2];
	PIC416Write(0x0F,DataByte);

	//delay the 4th clock bit of the 20bit command to allow programming....
	//use upper bits of 4bit command to configure the delay
	//18f24j50 needs 1.2ms, lower parts in same family need 3.2
	PIC416Write(0x40,0x0000);
}
//erase 18F, sleep delay should be adjustable
void PIC18_erase(uint16 key1, uint16 key2){
	PIC18_settblptr(0x3C0005); //set pinter to erase register
	PIC416Write(0x0C,key1);//write special erase token
	PIC18_settblptr(0x3C0004); //set pointer to second erase register
	PIC416Write(0x0C,key2);//write erase command
	PIC416Write(0,0);
	PIC416Write(0,0);
	sleep(1);//Thread.Sleep(1000); (524ms worst case)
}

void PIC24_erase(void){
	uint16 VISI;
	
	//set NVMCON
	PIC424Write(0x2404FA,0,0); //MOV XXXX,W10 0x404F (differs by PIC)
	PIC424Write(0x883B0A,0,0); //MOV W10,NVMCON

	// Step 3: Set TBLPAG
	PIC424Write(0x200000,0,0); //MOV XXXX,W0 (0x0000)
	PIC424Write(0x880190,0,1); //MOV W0,TABLPAG
	PIC424Write(0xBB0800,0,2); //TBLWTL W0,[W0] (dummy write)

	PIC424Write(0xA8E761,0,2); //BSET NVMCON,#WR
	//sleep(1);
	
	//repeat until erase done, should reset counter every now and then too... 11000000 01001111
	while(1){
		PIC24NOP();
		PIC424Write(0x803B02,0,2);//MOV NVMCON,W2
		PIC424Write(0x883C22,0,2);//MOV W2,VISI	

		VISI=PIC424Read();//REGOUT,0x000000,5, read VISI register (PIC includes 2 NOPS after every read, may need to be updated later)
		//printf("Read: %X \n",VISI); //REGOUT,0x000000,5, read VISI register
		PIC24NOP();//SIX,0x000000,5, N/A
		PIC424Write(0x040200,0,2);//SIX,0xBA0BB6,5, N/A TBLRDH.B [W6++], [W7++] (this needs a pre-NOP)

		if(!(VISI&0x8000)) break; //bit15 will clear when erase is done
	}

}

//18F setup write location and write length bytes of data to PIC
void PIC24_write(uint32 tblptr, uint16* Data, int length)
{
	uint16 DataByte;//, buffer[2]={0x00,0x00};
	int ctr;
	uint8	buffer[4] = {0};
	uint16 VISI;

	//set NVMCON
	PIC424Write(0x24001A,0,0); //MOV XXXX,W10 0x4001 (differs by PIC)
	PIC424Write(0x883B0A,0,0); //MOV W10,NVMCON
	
	//setup the table pointer
	PIC424Write(0x200000,0,0); //MOV XXXX,W0 (0x0000)
	PIC424Write(0x880190,0,1); //MOV W0,TABLPAG
	PIC424Write(0x200007,0,0); //MOV XXXX,W7 (0x0000)
	
	for(ctr=0;ctr<16;ctr++)
	{	
		PIC424Write(0x200000,0,0); //MOV XXXX,W0 (0x0000)
		PIC424Write(0x200001,0,0); //MOV XXXX,W1 (0x0000)
		PIC424Write(0x200002,0,0); //MOV XXXX,W2(0x0000)
		PIC424Write(0x200003,0,0); //MOV XXXX,W3 (0x0000)
		PIC424Write(0x200004,0,0); //MOV XXXX,W4 (0x0000)
		PIC424Write(0x200005,0,0); //MOV XXXX,W5 (0x0000)	

		PIC424Write(0xEB0300,0,1);//CLR W6		
		PIC424Write(0xBB0BB6,0,2);	//TBLWTL [W6++], [W7]
		PIC424Write(0xBBDBB6,0,2);	//TBLWTH.B [W6++], [W7++]
		PIC424Write(0xBBEBB6,0,2);	//TBLWTH.B [W6++], [++W7]
		PIC424Write(0xBB1BB6,0,2); //TBLWTL [W6++], [W7++]	
		PIC424Write(0xBB0BB6,0,2);	//TBLWTL [W6++], [W7]
		PIC424Write(0xBBDBB6,0,2);	//TBLWTH.B [W6++], [W7++]
		PIC424Write(0xBBEBB6,0,2);	//TBLWTH.B [W6++], [++W7]
		PIC424Write(0xBB1BB6,0,2);	//TBLWTL [W6++], [W7++]
	}
	
	//start the write cycle
	PIC424Write(0xA8E761,0,2);	//BSET NVMCON, #WR
		
	//repeat until write done, should reset counter every now and then too... 11000000 01001111
	while(1){
		PIC24NOP();
		PIC424Write(0x803B02,0,2);//MOV NVMCON,W2
		PIC424Write(0x883C22,0,2);//MOV W2,VISI	

		VISI=PIC424Read();//REGOUT,0x000000,5, read VISI register (PIC includes 2 NOPS after every read, may need to be updated later)
		//printf("Read: %X \n",VISI); //REGOUT,0x000000,5, read VISI register
		PIC24NOP();//SIX,0x000000,5, N/A
		PIC424Write(0x040200,0,2);//SIX,0xBA0BB6,5, N/A TBLRDH.B [W6++], [W7++] (this needs a pre-NOP)

		if(!(VISI&0x8000)) break; //bit15 will clear when done
	}
}

//
/* entry point */
//
enum { PIC614=0,
	   PIC416,
	   PIC424,
	};

int main (int argc, const char** argv)
{
	int		res = -1, picMode=PIC416;
	uint8	buffer[256] = {0};
	uint8	pages_used[PIC_NUM_PAGES] = {0};
	uint8*	bin_buff = NULL;
	uint8 i=0, PICrev;
	uint16 PICid, buffer16[256]={0};
	char PICname[]="UNKNOWN/INVALID PIC ID";
	
	puts("+++++++++++++++++++++++++++++++++++++++++++");
	puts("  piratePICprog for the Bus Pirate         ");
	puts("  Loader version: " PIRATE_LOADER_VERSION "  OS: " OS_NAME(OS));
	puts("+++++++++++++++++++++++++++++++++++++++++++\n");

	if( (res = parseCommandLine(argc, argv)) < 0 ) {
		return -1;
	} else if( res == 0 ) {
		return 0;
	}
	
	if( !g_hello_only ) {
		
		if( !g_hexfile_path ) {
			fprintf(stderr, "Please specify hexfile path --hex=/path/to/hexfile.hex\n");
			return -1;
		}
	
		bin_buff = (uint8*)malloc(256 << 10); //256kB
		if( !bin_buff ) {
			fprintf(stderr, "Could not allocate 256kB buffer\n");
			goto Error;
		}
		
		//fill the buffer with 0xFF
		memset(bin_buff, 0xFFFFFFFF, (256 << 10));
		
		printf("Parsing HEX file [%s]\n", g_hexfile_path);
		
		res = readHEX(g_hexfile_path, bin_buff, (256 << 8), pages_used);
		if( res <= 0 || res > PIC_FLASHSIZE ) {
			fprintf(stderr, "Could not load HEX file, result=%d\n", res);
			goto Error;
		}
		
		printf("Found %d words (%d bytes)\n", res, res * 3);
	
	}

	if( !g_device_path ) {
		fprintf(stderr, "Please specify serial device path --dev=/dev/...\n");
		return -1;
	}
	
	printf("Opening serial device %s...", g_device_path);
	
	dev_fd = openPort(g_device_path, 0);
	
	if( dev_fd < 0 ) {
		puts("ERROR");
		fprintf(stderr, "Could not open %s\n", g_device_path);
		goto Error;
	}
	puts("OK");
	
	printf("Configuring serial port settings...");
	
	if( configurePort(dev_fd, B115200) < 0 ) {
		puts("ERROR");
		fprintf(stderr, "Could not configure device, errno=%d\n", errno);
		goto Error;
	}
	puts("OK");
	
	
	//enter BBIO mode 

	printf("Entering binary mode...");
	sendString(dev_fd, 1, "\x00");
	res = readWithTimeout(dev_fd, buffer, 5, 1);
	if( memcmp(buffer, "BBIO1", 5) ) {
		for(i=0; i<20; i++){
			buffer[i]=0x00;
		}
		sendString(dev_fd, 19, buffer);
		
		res = readWithTimeout(dev_fd, buffer, 5, 1);
		if( memcmp(buffer, "BBIO1", 5) ) {
			puts("ERROR");
			goto Error;
		} 	
		//puts("(OK)");
	} 	
	printf(buffer);
	puts("(OK)");
	
	//enter raw-wire mode

	printf("Entering rawwire mode...");
	sendString(dev_fd, 1, "\x05");
	res = readWithTimeout(dev_fd, buffer, 4, 1);
	buffer[4]=0x00;
	printf(buffer);
	if( memcmp(buffer, "RAW1", 4) ) {
		puts("ERROR");
		goto Error;
	} 
	puts("(OK)");
	
	//setup output pin type (normal)
	printf("Setup mode...");
	if(writetopirate("\x8A")){
		puts("ERROR");
		goto Error;
	} 
	if(writetopirate("\x63")){ //high speed mode
		puts("ERROR");
		goto Error;
	} 
	
	puts("(OK)");
	
	//setup power supply, AUX pin, pullups
	printf("Setup peripherals...");
	if(writetopirate("\x4F")){
		puts("ERROR");
		goto Error;
	} 
	puts("(OK)");
	
	switch(picMode){
		case PIC416:
			//setup rawwire for the correct PIC type
			printf("Set 4/16 programming mode...");
			sendString(dev_fd, 1, "\xA0");
			if(writetopirate("\x01")){
				puts("ERROR");
				goto Error;
			} 
			puts("(OK)");
			
			//enter ICSP
			puts("Entering ICSP...");
			PIC18_enterLowVPPICSP(0x4D434850);//key should be part of device info
			
			//now we're in ICSP mode, can do programming stuff with the PIC
			//read ID
			PICid=PIC18_readID(0x3ffffe); //give it the ID address

			//determine device type
			PICrev=(PICid&(~0xFFE0)); //find PIC ID (lower 5 bits)
			PICid=(PICid>>5); //isolate PIC device ID (upper 11 bits)

			//could this be an array/lookup table or part of a type info struct???
			switch(PICid){
				case 0x260:
					strcpy(PICname,"18F24J50"); 
					break;
			}	
			printf ("PIC ID: %#X (%s) REV: %#X \n", PICid, PICname, PICrev);
			
			//erase device
			printf("Erasing the PIC (please wait)...");
			PIC18_erase(0x0101,0x8080);
			puts("(OK)");
			
			//exit programming mode
			puts("Exit ICSP...");
			exitICSP();
			
			//write a HEX is there is one
			if( !g_hello_only ) {

				//enter ICSP mode
				puts("Entering ICSP...");
				PIC18_enterLowVPPICSP(0x4D434850); //key should be part of device info
		  
				//some initial programming setup stuff, enable page writes, etc
				PIC18_setupwrite();
			
				//write the firmware to the PIC 
				res = PIC18_sendFirmware(dev_fd, bin_buff, pages_used);
				
				//exit ICSP
				puts("Exit ICSP...");
				exitICSP();

				//puts("Read PIC test...");
				
				//puts("Entering ICSP...");
				//PIC18_enterLowVPPICSP(0x4D434850); //key should be part of device info

				//read the PIC
				//PIC18_read(0x000000, bin_buff, 0xff);
				//dumpHex(bin_buff,0xff); //dump to screen
				
				//exit ICSP
				//puts("Exit ICSP...");
				//exitICSP();
						
				if( res > 0 ) {
					puts("\nFirmware updated successfully :)!");
				} else {
					puts("\nError updating firmware :(");
					goto Error;
				}
				
			}
			break;
		case PIC424:
			printf("Set 4/24 programming mode...");
			sendString(dev_fd, 1, "\xA0");
			if(writetopirate("\x02")){
				puts("ERROR");
				goto Error;
			} 
			puts("(OK)");
			
			//enter ICSP
			puts("Entering ICSP...");
			PIC24_enterLowVPPICSP(0x4D434851);//key should be part of device info
			
			//now we're in ICSP mode, can do programming stuff with the PIC
			//read ID
			PIC24_read(0x00FF0000, buffer16, 2); //give it the ID address
			//printf("Read: %X \n",buffer16[0]); //first byte is the ID
			//printf("Read: %X \n",buffer16[1]); //second byte is major.minor revision

			//determine device type
			//PICrev=(PICid&(~0xFFE0)); //find PIC ID (lower 5 bits)
			//PICid=(); //isolate PIC device ID (upper 11 bits)

			//could this be an array/lookup table or part of a type info struct???
			switch(buffer16[0]){
				case 0x0447:
					strcpy(PICname,"24FJ64GA002"); 
					break;
			}	
			printf ("PIC ID: %#X (%s) REV: %#X (%#d.%#d) \n", buffer16[0], PICname, buffer16[1], ((buffer16[1]>>6)&0x07),(buffer16[1]&0x07));
			
			//erase device
			printf("Erasing the PIC (please wait)...");
			PIC24_erase();
			puts("(OK)");
			
			//exit programming mode
			puts("Exit ICSP...");
			exitICSP();
			
			//write a HEX is there is one
			if( !g_hello_only ) {

				//enter ICSP mode
				puts("Entering ICSP...");
				PIC24_enterLowVPPICSP(0x4D434851); //key should be part of device info
		
				//write the firmware to the PIC 
				//res = sendFirmware(dev_fd, bin_buff, pages_used);
				puts("Programming first page with 0x00...");
				PIC24_write(0x0000, buffer16, 64); //currently all 3 variables are ignored
				//exit ICSP
				puts("Exit ICSP...");
				exitICSP();

				puts("Read PIC test...");
				
				puts("Entering ICSP...");
				PIC24_enterLowVPPICSP(0x4D434851); //key should be part of device info

				//read the PIC
				PIC24_read(0x00000000, buffer16, 0xA); //give it the ID address
				//dumpHex(buffer16,0xA); //dump to screen
				
				//exit ICSP
				puts("Exit ICSP...");
				exitICSP();
						
				if( res > 0 ) {
					puts("\nFirmware updated successfully :)!");
				} else {
					puts("\nError updating firmware :(");
					goto Error;
				}
				
			}
			
			
			
			
			break;
		
	}
	

Finished:
	puts("Done!");
	if( bin_buff ) { 
		free( bin_buff );
	}
	close(dev_fd);
    return 0;
	
Error:
	if( bin_buff ) {
		free( bin_buff );
	}
	if( dev_fd >= 0 ) {
		close(dev_fd);
	}
	return -1;
}
