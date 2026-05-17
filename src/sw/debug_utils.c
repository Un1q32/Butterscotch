#include <stdio.h>
#include <windows.h>

int debug_fprintf(FILE* stream, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	if (stream == stderr)
	{
		static char buffer[4096];
		int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
		
		OutputDebugStringA(buffer);
		
		FILE* stderr2 = fopen("log.txt", "a+");
		fwrite(buffer, 1, strlen(buffer), stderr2);
		fclose(stderr2);
		
		va_end(args);
		return len;
	}

	int len = vfprintf(stream, fmt, args);
	va_end(args);
	return len;
}

int debug_printf(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	static char buffer[4096];
	int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
	OutputDebugStringA(buffer);
	
	FILE* stderr2 = fopen("log.txt", "a+");
	fwrite(buffer, 1, strlen(buffer), stderr2);
	fclose(stderr2);
	
	va_end(args);
	return len;
}
