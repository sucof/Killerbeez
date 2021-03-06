#include <global_types.h>
#include <driver.h>
#include <driver_factory.h>
#include <mutator_factory.h>
#include <instrumentation.h>
#include <instrumentation_factory.h>
#include <utils.h>

#include <io.h>
#include <Shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define F_OK 00
#define W_OK 02

/**
 * This function prints out the usage information for the fuzzer and each of the individual components
 * @param program_name - the name of the program currently being run (for use in the outputted message)
 * @param mutator_directory - the directory to look for mutators in, when printing out the mutator help information
 */
void usage(char * program_name, char * mutator_directory)
{
	char * help_text;
	printf(
		"Usage: %s driver_name instrumentation_name mutator_name [options]\n"
		"\n"
		"Options:\n"
		"\t -d driver_options                 Set the options for the driver\n"
		"\t -i instrumentation_options        Set the options for the instrumentation\n"
		"\t -isd instrumentation_state_file   Set the file containing that the instrumentation state should dump to\n"
		"\t -isf instrumentation_state_file   Set the file containing that the instrumentation state should load from\n"
		"\t -l logging_options                Set the options for logging\n"
		"\t -n num_iterations                 The number of iterations to run [infinite]\n"
		"\t -m mutator_options                Set the options for the mutator\n"
		"\t -md mutator_directory             The directory to look for mutator DLLs in (must be specified to view help for specific mutators)\n"
		"\t -ms mutator_state                 Set the state that the mutator should load\n"
		"\t -msd mutator_state_file           Set the file containing that the mutator state should dump to\n"
		"\t -msf mutator_state_file           Set the file containing that the mutator state should load from\n"
		"\t -o output_directory               The directory to write files which cause a crash or hang\n"
		"\t -sf seed_file                     The seed file to use\n"
		"\n\n",
		program_name
	);

#define PRINT_HELP(x, y) \
	x = y;               \
	if(x) {              \
		puts(x);         \
		free(x);         \
	}

	PRINT_HELP(help_text, logging_help());
	PRINT_HELP(help_text, driver_help());
	PRINT_HELP(help_text, instrumentation_help());
	PRINT_HELP(help_text, mutator_help(mutator_directory));
		
	exit(1);
}

int main(int argc, char ** argv)
{
	driver_t * driver;
	mutator_t * mutator;
	instrumentation_t * instrumentation;
	char *driver_name, *driver_options = NULL,
		*mutator_name, *mutator_options = NULL, *mutator_saved_state = NULL, *mutation_state_dump_file = NULL, *mutation_state_load_file = NULL,
		*mutate_buffer = NULL, *mutator_directory = NULL, *mutator_directory_cli = NULL,
		*logging_options = NULL,
		*seed_file = NULL, *seed_buffer = NULL,
		*instrumentation_name = NULL, *instrumentation_options = NULL, 
		*instrumentation_state_string = NULL, *instrumentation_state_load_file = NULL,
		*instrumentation_state_dump_file = NULL;
	void * instrumentation_state;
	int seed_length = 0, mutate_length = 0, instrumentation_length = 0, mutator_state_length;
	time_t fuzz_begin_time;
	int iteration = 0, status, new_path;
	void * mutator_state = NULL;
	char filename[MAX_PATH];
	char filehash[256];
	char * directory;

	//Default options
	int num_iterations = 1;
	char * output_directory = "output";

	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// Mutator Setup /////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////

	if (!mutator_directory)
	{
		char * mutator_repo_dir = getenv("KILLERBEEZ_MUTATORS");
		if (mutator_repo_dir) //If the environment variable KILLERBEEZ_MUTATORS is set, try to autodetect the directory based on the repo build path
		{
			mutator_directory = (char *)malloc(MAX_PATH + 1);
			if (!mutator_directory)
			{
				printf("Couldn't get memory for default mutator_directory");
				return 1;
			}
			memset(mutator_directory, 0, MAX_PATH + 1);
#if defined(_M_X64) || defined(__x86_64__)
#ifdef _DEBUG
			snprintf(mutator_directory, MAX_PATH, "%s\\..\\build\\x64\\Debug\\mutators\\", mutator_repo_dir);
#else
			snprintf(mutator_directory, MAX_PATH, "%s\\..\\build\\x64\\Release\\mutators\\", mutator_repo_dir);
#endif
#else
#ifdef _DEBUG
			snprintf(mutator_directory, MAX_PATH, "%s\\..\\build\\Debug\\mutators\\", mutator_repo_dir);
#else
			snprintf(mutator_directory, MAX_PATH, "%s\\..\\build\\Release\\mutators\\", mutator_repo_dir);
#endif
#endif
		}
		else
		{
			mutator_directory = filename_relative_to_binary_dir("..\\mutators\\");
		}
	}
	
	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// Parse Arguments ///////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////

	if (argc < 4)
	{
		usage(argv[0], mutator_directory);
	}

	driver_name = argv[1];
	instrumentation_name = argv[2];
	mutator_name = argv[3];

	//Now parse the rest of the args now that we have a valid mutator dir setup
	for (int i = 4; i < argc; i++)
	{
		IF_ARG_OPTION("-d", driver_options)
		ELSE_IF_ARG_OPTION("-i", instrumentation_options)
		ELSE_IF_ARG_OPTION("-isd", instrumentation_state_dump_file)
		ELSE_IF_ARG_OPTION("-isf", instrumentation_state_load_file)
		ELSE_IF_ARGINT_OPTION("-n", num_iterations)
		ELSE_IF_ARG_OPTION("-m", mutator_options)
		ELSE_IF_ARG_OPTION("-md", mutator_directory_cli)
		ELSE_IF_ARG_OPTION("-l", logging_options)
		ELSE_IF_ARG_OPTION("-ms", mutator_saved_state)
		ELSE_IF_ARG_OPTION("-msd", mutation_state_dump_file)
		ELSE_IF_ARG_OPTION("-msf", mutation_state_load_file)
		ELSE_IF_ARG_OPTION("-o", output_directory)
		ELSE_IF_ARG_OPTION("-sf", seed_file)
	    else
		{
			if (strcmp("-h", argv[i]))
				printf("Unknown argument: %s\n", argv[i]);
			usage(argv[0], mutator_directory);
		}
	}

	if (setup_logging(logging_options))
	{
		printf("Failed setting up logging, exiting\n");
		return 1;
	}

	//Check number of iterations for valid number of rounds
	if (num_iterations <= 0)
		FATAL_MSG("Invalid number of iterations %d", num_iterations);

	if (mutator_directory_cli) 
	{ 
		free(mutator_directory);
		mutator_directory = strdup(mutator_directory_cli); 
		mutator_directory_cli = NULL;
	}
	if (!mutator_directory)
		FATAL_MSG("Mutator directory was not found in default location. You may need to pass the -md flag.");

	if (instrumentation_state_dump_file) {
		strncpy(filename, instrumentation_state_dump_file, sizeof(filename));
		PathRemoveFileSpec(filename);
		if (access(filename, W_OK))
			FATAL_MSG("The provided instrumentation_state_dump_file filename (%s) is not writeable", instrumentation_state_dump_file);
	}
	if (mutation_state_dump_file) {
		strncpy(filename, mutation_state_dump_file, sizeof(filename));
		PathRemoveFileSpec(filename);
		if (access(filename, W_OK))
			FATAL_MSG("The provided mutation_state_dump_file filename (%s) is not writeable", mutation_state_dump_file);
	}

#define create_output_directory(name)                                                \
	snprintf(filename, sizeof(filename), "%s" name, output_directory);               \
	if(!CreateDirectory(filename, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { \
		FATAL_MSG("Unable to create directory %s", filename);                        \
		return 1;                                                                    \
	}

	//Setup the output directory
	create_output_directory("");
	create_output_directory("/crashes");
	create_output_directory("/hangs");
	create_output_directory("/new_paths");

	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// Ojbect Setup //////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////

	//Load the instrumentation state from disk (if specified, and create the instrumentation
	if (instrumentation_state_load_file)
	{
		instrumentation_length = read_file(instrumentation_state_load_file, &instrumentation_state_string);
		if (instrumentation_length <= 0)
			FATAL_MSG("Could not read instrumentation file or empty instrumentation file: %s", instrumentation_state_load_file);
	}
	instrumentation = instrumentation_factory(instrumentation_name);
	if (!instrumentation)
	{
		free(instrumentation_state_string);
		FATAL_MSG("Unknown instrumentation '%s'", instrumentation_name);
	}
	instrumentation_state = instrumentation->create(instrumentation_options, instrumentation_state_string);
	if (!instrumentation_state)
	{
		free(instrumentation_state_string);
		FATAL_MSG("Bad options/state for instrumentation %s", instrumentation_name);
	}
	free(instrumentation_state_string);

	//Load the seed buffer from a file
	if (seed_file)
	{
		seed_length = read_file(seed_file, &seed_buffer);
		if (seed_length <= 0)
			FATAL_MSG("Could not read seed file or empty seed file: %s", seed_file);
	}
	if (!seed_buffer)
		FATAL_MSG("No seed file or seed id specified.");

	if (mutation_state_load_file)
	{
		free(mutator_saved_state);
		mutator_state_length = read_file(mutation_state_load_file, &mutator_saved_state);
		if (mutator_state_length <= 0)
			FATAL_MSG("Could not read mutator saved state from file: %s", mutation_state_load_file);
	}

	//Create the mutator
	mutator = mutator_factory_directory(mutator_directory, mutator_name);
	if (!mutator)
		FATAL_MSG("Unknown mutator (%s)", mutator_name);
	free(mutator_directory);
	mutator_state = mutator->create(mutator_options, mutator_saved_state, seed_buffer, seed_length);
	if (!mutator_state)
		FATAL_MSG("Bad mutator options or saved state for mutator %s", mutator_name);
	free(mutator_saved_state);
	free(seed_buffer);

	//Create the driver
	driver = driver_all_factory(driver_name, driver_options, instrumentation, instrumentation_state, mutator, mutator_state);
	if (!driver)
		FATAL_MSG("Unknown driver '%s' or bad options: \ndriver options: %s\nmutator options: %s\n", driver_name, driver_options, mutator_options);

	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// Main Fuzz Loop ////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////

	fuzz_begin_time = time(NULL);

	//Copy the input, mutate it, and run the fuzzed program
	for (iteration = 0; iteration < num_iterations; iteration++)
	{
		DEBUG_MSG("Fuzzing the %d iteration", iteration);
		status = driver->test_next_input(driver->state);
		if (status < 0)
		{
			if(status == -2)
				WARNING_MSG("The mutator has run out of mutations to test after %d iterations", iteration);
			else
				ERROR_MSG("ERROR: driver failed to test the target program");
			break;
		}

		new_path = instrumentation->is_new_path(instrumentation_state, &status);
		if (new_path < 0)
		{
			printf("ERROR: instrumentation failed to determine the fuzzed process's status\n");
			break;
		}

		directory = NULL;
		if (status == FUZZ_CRASH)
			directory = "crashes";
		else if (status == FUZZ_HANG)
			directory = "hangs";
		else if (new_path > 0)
			directory = "new_paths";

		if (directory != NULL)
		{
			CRITICAL_MSG("Found %s", directory);

			mutate_buffer = driver->get_last_input(driver->state, &mutate_length);
			if (!mutate_buffer)
				ERROR_MSG("Unable to dump mutate buffer\n");
			else
			{
				if (output_directory) {
					md5((uint8_t *)mutate_buffer, mutate_length, filehash, sizeof(filehash));
					snprintf(filename, MAX_PATH, "%s/%s/%s", output_directory, directory, filehash);
					if (access(filename, F_OK)) //If the file already exists, there's no reason to write it again
						write_buffer_to_file(filename, mutate_buffer, mutate_length);
				}
				free(mutate_buffer);
			}
		}
	}

	INFO_MSG("Ran %ld iterations in %I64d seconds", iteration, time(NULL) - fuzz_begin_time);

	//////////////////////////////////////////////////////////////////////////////////////////////////////
	// Cleanup ///////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////////

	if (instrumentation_state_dump_file)
	{
		instrumentation_state_string = instrumentation->get_state(instrumentation_state);
		if (instrumentation_state_string)
		{
			write_buffer_to_file(instrumentation_state_dump_file, instrumentation_state_string, strlen(instrumentation_state_string));
			instrumentation->free_state(instrumentation_state_string);
		}
		else
			WARNING_MSG("Couldn't dump instrumentation state to file %s", instrumentation_state_dump_file);
	}
	if (mutation_state_dump_file)
	{
		mutator_saved_state = mutator->get_state(mutator_state);
		if (mutator_saved_state)
		{
			write_buffer_to_file(mutation_state_dump_file, mutator_saved_state, strlen(mutator_saved_state));
			mutator->free_state(mutator_saved_state);
		}
		else
			WARNING_MSG("Couldn't dump mutator state to file %s", mutation_state_dump_file);
	}

	//Cleanup everything and exit
	driver->cleanup(driver->state);
	instrumentation->cleanup(instrumentation_state);
	mutator->cleanup(mutator_state);
	free(driver);
	free(instrumentation);
	free(mutator);
	return 0;
}
