// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static int currentColor = 0x0700;
static int clrs[16] = {
	0x0000,
	0x0000, 
    0x0100,
	0x1000, 
    0x0200,
    0x2000,  
    0x0300,
    0x3000,    
    0x0400,
    0x4000,   
    0x0500,
    0x5000,    
    0x0600,
    0x6000,  
    0x0700,
    0x7000 };

void getColor();
void getBrightColor();
void repaint();
void repaintLastRow();

static void consputc(int);

static int panicked = 0;

static struct {
	struct spinlock lock;
	int locking;
} cons;

static void
printint(int xx, int base, int sign)
{
	static char digits[] = "0123456789abcdef";
	char buf[16];
	int i;
	uint x;

	if(sign && (sign = xx < 0))
		x = -xx;
	else
		x = xx;

	i = 0;
	do{
		buf[i++] = digits[x % base];
	}while((x /= base) != 0);

	if(sign)
		buf[i++] = '-';

	while(--i >= 0)
		consputc(buf[i]);
}

// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
	int i, c, locking;
	uint *argp;
	char *s;

	locking = cons.locking;
	if(locking)
		acquire(&cons.lock);

	if (fmt == 0)
		panic("null fmt");

	argp = (uint*)(void*)(&fmt + 1);
	for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
		if(c != '%'){
			consputc(c);
			continue;
		}
		c = fmt[++i] & 0xff;
		if(c == 0)
			break;
		switch(c){
		case 'd':
			printint(*argp++, 10, 1);
			break;
		case 'x':
		case 'p':
			printint(*argp++, 16, 0);
			break;
		case 's':
			if((s = (char*)*argp++) == 0)
				s = "(null)";
			for(; *s; s++)
				consputc(*s);
			break;
		case '%':
			consputc('%');
			break;
		default:
			// Print unknown % sequence to draw attention.
			consputc('%');
			consputc(c);
			break;
		}
	}

	if(locking)
		release(&cons.lock);
}

void
panic(char *s)
{
	int i;
	uint pcs[10];

	cli();
	cons.locking = 0;
	// use lapiccpunum so that we can call panic from mycpu()
	cprintf("lapicid %d: panic: ", lapicid());
	cprintf(s);
	cprintf("\n");
	getcallerpcs(&s, pcs);
	for(i=0; i<10; i++)
		cprintf(" %p", pcs[i]);
	panicked = 1; // freeze other CPU
	for(;;)
		;
}

#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

static void
cgaputc(int c)
{
	int pos;

	// Cursor position: col + 80*row.
	outb(CRTPORT, 14);
	pos = inb(CRTPORT+1) << 8;
	outb(CRTPORT, 15);
	pos |= inb(CRTPORT+1);

	if(c == '\n')
		pos += 80 - pos%80;
	else if(c == BACKSPACE){
		if(pos > 0) --pos;
	} else
		crt[pos++] = (c&0xff) | currentColor;  // black on white

	if(pos < 0 || pos > 25*80)
		panic("pos under/overflow");

	if((pos/80) >= 24){  // Scroll up.
		memmove(crt, crt+80, sizeof(crt[0])*23*80);
		pos -= 80;
		memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
		if(currentColor != 0x0700) repaintLastRow();
	}

	outb(CRTPORT, 14);
	outb(CRTPORT+1, pos>>8);
	outb(CRTPORT, 15);
	outb(CRTPORT+1, pos);
	crt[pos] = ' ' | currentColor;
}

void
consputc(int c)
{
	if(panicked){
		cli();
		for(;;)
			;
	}

	if(c == BACKSPACE){
		uartputc('\b'); uartputc(' '); uartputc('\b');
	} else
		uartputc(c);
	cgaputc(c);
}

#define INPUT_BUF 128
struct {
	char buf[INPUT_BUF];
	uint r;  // Read index
	uint w;  // Write index
	uint e;  // Edit index
} input;

#define C(x)  ((x)-'@')  // Control-x
#define A(x) (x + 100)  // Alt

void showMenu();
void saveBackground();
void loadBackground();
static char backgroundBackup[230];

void handleInput(char c);
void displaySelection(int cur);
int computeLowerBound(int x);

static int flagC = 0;
static int flagO = 0;

// 1 - active ... 0 - hidden
static int menuStatus = 0;

// 0 - Default 
static int currentSelection = 0;

void clearFlags(){
	flagC = 0;
	flagO = 0;
}

void
consoleintr(int (*getc)(void))
{
	int c, doprocdump = 0;

	acquire(&cons.lock);
	while((c = getc()) >= 0){
		switch(c){
		case C('P'):  // Process listing.
			// procdump() locks cons.lock indirectly; invoke later
			doprocdump = 1;
			clearFlags();
			break;
		case C('U'):  // Kill line.
			while(input.e != input.w &&
			      input.buf[(input.e-1) % INPUT_BUF] != '\n'){
				input.e--;
				consputc(BACKSPACE);
			}
			clearFlags();
			break;
		case C('H'): case '\x7f':  // Backspace
			if(!menuStatus){
				if(input.e != input.w){
				input.e--;
				consputc(BACKSPACE);
				}	
			}
			clearFlags();
			break;
		case A('c'):			
			flagC = 1;
			flagO = 0;
			break;
		case A('o'):
			if(flagO){
				clearFlags();
				break;
				} 	// stops alt c-o-o-l sequence
			flagC? flagO = 1:clearFlags(); 
			break;
		case A('l'):
			if(flagC && flagO){
				menuStatus? loadBackground():saveBackground();
				menuStatus =! menuStatus;
			}
			clearFlags();
			break;
		default: // Da li kombinacija tastera koja nema ispis prekida sekvencu???
			if(!menuStatus){			
				if(c != 0 && input.e-input.r < INPUT_BUF){
					clearFlags();
					c = (c == '\r') ? '\n' : c;
					input.buf[input.e++ % INPUT_BUF] = c;
					consputc(c);
					if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
						input.w = input.e;
						wakeup(&input.r);
					}
				}
			}else { 	
				handleInput(c);
				showMenu(); // clears last selected row
				displaySelection(currentSelection);
				if(c != 0) clearFlags(); // bez ovoga ne radi zatvaranje menija, prolazi alt cXXXXol 
				//clearFlags(); // ne radi menu close, jedino ako razmak skoro ne postoji izmedju c-o-l
			}
			break;
		}
	}
	release(&cons.lock);
	if(doprocdump) {
		procdump();  // now call procdump() wo. cons.lock held
	}
}

int
consoleread(struct inode *ip, char *dst, int n)
{
	uint target;
	int c;

	iunlock(ip);
	target = n;
	acquire(&cons.lock);
	while(n > 0){
		while(input.r == input.w){
			if(myproc()->killed){
				release(&cons.lock);
				ilock(ip);
				return -1;
			}
			sleep(&input.r, &cons.lock);
		}
		c = input.buf[input.r++ % INPUT_BUF];
		if(c == C('D')){  // EOF
			if(n < target){
				// Save ^D for next time, to make sure
				// caller gets a 0-byte result.
				input.r--;
			}
			break;
		}
		*dst++ = c;
		--n;
		if(c == '\n')
			break;
	}
	release(&cons.lock);
	ilock(ip);

	return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
	int i;

	iunlock(ip);
	acquire(&cons.lock);
	for(i = 0; i < n; i++)
		consputc(buf[i] & 0xff);
	release(&cons.lock);
	ilock(ip);

	return n;
}

void
consoleinit(void)
{
	initlock(&cons.lock, "console");

	devsw[CONSOLE].write = consolewrite;
	devsw[CONSOLE].read = consoleread;
	cons.locking = 1;

	ioapicenable(IRQ_KBD, 0);
}


/*


/---<FG>--- ---<BG>---\		   Index bounds
|Black     |Black     |	    138,148 | 149,159   
|Blue      |Blue      |		218,228 | 229,239		
|Green     |Green     |
|Aqua      |Aqua      |
|Red       |Red       |
|Purple    |Purple    |
|Yellow    |Yellow    |
|White     |White     |
\---------------------/



Bound calculator 
----------------
startingbound = x % 2 > 0 ? 149:138;
lowerbound = 0;
lowerbound = startingbound + (x/2) * 80
upperbound = lowerbound + 10



/-FG--- -BG---\
|(0)   |(1)   | 
|(2)   |(3)   |
|(4)   |(5)   |			  (0)  ,    (1)  ,    (2) ,   (3)
|(6)   |(7)   |  - - - > [BlackFG, BlackBG, BlueFG, BlueBG,...]
|(8)   |(9)   |
|(10)  |(11)  |
|(12)  |(13)  |
|(14)  |(15)  |
\-------------/

*/

static char menustr[] = "/---<FG>--- ---<BG>---\\|Black     |Black     ||Blue      |Blue      ||Green     |Green     ||Aqua      |Aqua      ||Red       |Red       ||Purple    |Purple    ||Yellow    |Yellow    ||White     |White     |\\---------------------/";

void showMenu(){
int startingPosition = 57;
for(int i = 0; i < 230; i++){
		if(i % 23 == 0){
			startingPosition = 57 + 80 * (i / 23);
		}
		crt[startingPosition] = (menustr[i]&0xff) | 0x0f00;
		startingPosition++;
		}
			
}


void saveBackground(){
int startingPosition = 57;
for(int i = 0; i < 230; i++){
		if(i % 23 == 0){
			startingPosition = 57 + 80 * (i / 23);
		}
		backgroundBackup[i] = crt[startingPosition];
		startingPosition++;
		}
}


void loadBackground(){
int startingPosition = 57;
for(int i = 0; i < 230; i++){
		if(i % 23 == 0){
			startingPosition = 57 + 80 * (i / 23);
		}
		crt[startingPosition] = backgroundBackup[i] | currentColor;
		startingPosition++;
		}

}


void handleInput(char c){
	switch(c){
	case('s'):
		currentSelection = (currentSelection + 2) % 16;
		break;
	case('w'):
		currentSelection = (currentSelection - 2 + 16) % 16;
		break;
	case('d'):case('a'):
		currentSelection = currentSelection % 2 ? (currentSelection - 1):(currentSelection + 1);
		break;
	case('e'):
		getColor(currentSelection);
		repaint(); 
		break;
	case('r'):
		getBrightColor(currentSelection);
		repaint(); 
		break;	
	}

}


void displaySelection(int cur){
	int lowerbound = computeLowerBound(cur);
	for(int i = lowerbound; i < lowerbound + 10; i++){
		crt[i] = (crt[i]&0xff) | 0xf000;
	}
}


int computeLowerBound(int x){
	int startingbound = x % 2 ? 149:138;
	return startingbound + (x/2) * 80;
}									


void getColor(){
	if(currentSelection % 2 ){
		currentColor = (currentColor & 0x0fff) | clrs[currentSelection];
	}else{
		currentColor = (currentColor & 0xf0ff) | clrs[currentSelection];
	}
}


void getBrightColor(){
	if(currentSelection % 2 ){ 
		currentColor = (currentColor & 0x0fff) | (clrs[currentSelection] + 0x8000);
	}else{
		currentColor = (currentColor & 0xf0ff) | (clrs[currentSelection] + 0x0800);
	}
}


void repaint(){
	for(int i=0; i < 2000; i++){
		crt[i] = (crt[i]&0xff) | currentColor;
	}
}

void repaintLastRow(){
	for(int i=1840; i < 1920; i++){
		crt[i] = (crt[i]&0xff) | currentColor;
	}
}