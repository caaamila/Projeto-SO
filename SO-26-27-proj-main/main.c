#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
//o scandir faz malloc ent temos q fazer free
// o sccandir cria as struct dirent 
// ele lê a diretoria, aloca memória para cada entrada e entrega no fim
// o scandir devolve o numero de ficheiros q obedece ao filtro 

//  struct dirent {
//               ino_t          d_ino;       /* Inode number */
//               off_t          d_off;       /* Not an offset; see below */
//               unsigned short d_reclen;    /* Length of this record */
//               unsigned char  d_type;      /* Type of file; not supported by all filesystem types */
//               char           d_name[256]; /* Null-terminated filename */
//  };

static int filtro(const struct dirent *e) {
    const char *ponto = strrchr(e->d_name, '.');
    return ponto != NULL && strcmp(ponto, ".conf") == 0;
}


int main(int argc, char **argv){
	DataCenter dc;
	datacenter_init(&dc);

	if (argc != 6) {
		fprintf(stderr, "Usage: %s <servers> <ram> <disk> <cpus> <input_dir>\n", argv[0]);
		return 1;
	}

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	char dir[MAX_PATH_SIZE];
	
	if (parse_size_t_arg(argv[1], &servers) != 0 ||
			parse_size_t_arg(argv[2], &ram) != 0 ||
			parse_size_t_arg(argv[3], &disk) != 0 ||
			parse_double_arg(argv[4], &cpu) != 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	if(strlen(argv[5])>=MAX_PATH_SIZE){
		printf("Nome da diretoria demasiado longo");
		return 1;
	}

	strcpy(dir, argv[5]);

	Resources resources = {
    .ram = ram,
    .disk = disk,
    .cpu = cpu
	};

	if(datacenter_configure(&dc, servers, &resources) != 0){
		fprintf(stderr, "Failed to configure Data Center.\n");
		return 1;
	}

	struct dirent **lista;
	int n = scandir(dir, &lista, filtro, alphasort); //devolve o numero de ficheiros q obedecem ao filtro
	if (n == -1) {
		perror("scandir");
		return 1;
	}
	
	for(int i=0; i<n; i++){

		//construir caminho para o open

		char caminho[MAX_PATH_SIZE+ 1 + 256];
		snprintf(caminho, sizeof(caminho), "%s/%s", dir, lista[i]->d_name);
		//dir agora contem o caminho
		int fd=open(caminho, O_RDONLY);

		if(fd==-1){
			perror("Erro ao abrir");
			return EXIT_FAILURE;
		}

		int fim=0; //0 false ou seja nao acabou

		while(!fim){
			switch (get_next_command(fd)){
				case CMD_DEFINE: {
					VMType vmtype;

					if (parse_define(fd, &vmtype) != 0) {
						fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
						continue;
					}

					if(datacenter_define_VM(&dc, &vmtype) != 0){
						fprintf(stderr, "Failed to define VM.\n");
						continue;
					}

					printf("VM successfully defined!\n");

					break;
				}

				case CMD_RESERVE: {
					Reservation reservation = {0};

					size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

					if (num_items == 0) {
						fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
						continue;
					}

					if (datacenter_reserve(&dc, &reservation) != 0) {
						fprintf(stderr, "Failed to reserve VMs.\n");
						continue;
					}

					printf("Reservation made successfully!\n");

					break;
				}

				case CMD_EXECUTE:
					char id[MAX_STRING_SIZE];

					if(parse_execute(fd, id) != 0){
						fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
						continue;
					}

					if (datacenter_execute(&dc, id) != 0) {
						fprintf(stderr, "Failed to execute reservation.\n");
						continue;
					}

					printf("Finished reservation execution!\n");

					break;

				case CMD_LIST:
					if (datacenter_list(&dc) != 0) {
						fprintf(stderr, "Failed to list VMs.\n");
						continue;
					}

					break;

				case CMD_WAIT:
					unsigned int delay;

					if(parse_wait(fd, &delay) != 0){
						fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
						continue;
					}

					datacenter_wait(delay);
					break;

				case CMD_INVALID:
					fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
					break;

				case CMD_HELP:
					printf(
						"Spaces between arguments are allowed, but not after command end.\n"
						"Available commands:\n"
						" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
						" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
						" A <RESERVATION_ID>\n"
						" L\n"
						" E <DELAY_MS>\n"
						" H\n"
					);
					break;

				case CMD_EMPTY:
					break;

				case EOC:
					fim=1;
					break;
			}
		}

		close(fd);

		free(lista[i]);
	}
	
	datacenter_destroy(&dc);
	free(lista);
}