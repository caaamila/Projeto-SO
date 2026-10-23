#include "datacenter_utils.h"
#include "filesystem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h> 
#include <sys/types.h>

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

/**
 * Checks if a reservation with the given id already exists in the Data Center.
 * 
 * @param dc Pointer to Data Center.
 * @param reservation_id Id of the reservation being checked.
 * 
 * @return 1 if the reservation exists.
 * @return 0 if the reservation does not exist.
 */	
static int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	if(reservation_exists(dc, reservation->id)){
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		return 1;
	}

	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	/* Copy current server resources and hosted_vms so we can simulate the allocation. */
	for (size_t i = 0; i < dc->num_servers; i++) {
		available[i] = dc->servers[i].available;
		hosted_count[i] = dc->servers[i].num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			return 1;
		}

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        return 1;
    	}

			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list
		server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

		// Remove VM from reservation
		reservation->num_vms--;

		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}

int reservation_commit(DataCenter *dc, Reservation *reservation){
	// Keep track of how many vms existed before.
	size_t initial_vms = reservation->num_vms;

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id, MAX_VM_ID_STRING, "%s_%zu", reservation->id, reservation->num_vms + 1);

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;

		}
	}

	reservation->state = RES_STATE_PENDING;

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	// Free every VM belonging to the reservation.
	for (size_t i = 0; i < reservation->num_vms; i++) {
		VM *vm = reservation->vms[i];
		Server *server = vm->server;

		// Return the VM resources back to the hosting server.
		resources_add(&server->available, vm->type->required);

		// Remove the VM from the server's hosted VM list.
		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				// Shift the remaining VMs one position to the left.
				memmove(&server->hosted_vms[j],
								&server->hosted_vms[j + 1],
								(server->num_hosted_vms - j - 1) * sizeof(VM *));

				server->num_hosted_vms--;
				break;
			}
		}

		free(vm);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;

	// Find the reservation inside the Data Center reservation list.
	size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

	// Reservation not found (should never happen).
	if (idx == dc->num_reservations)
		return;

	// Remove the reservation by shifting the remaining ones.
	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}


void spawn_vm_child(VM *vm, Reservation *reservation){
 
	// coloca se esta parte aqui para o pai nao estar ocupado a fazer a copia
	// entao a child encarrega-se disso para deixar o pai fazer coisas mais importantes
	char vm_dir[MAX_PATH_SIZE];
	char res_dir[MAX_PATH_SIZE];
	snprintf(res_dir, sizeof(res_dir), "/tmp/CloudIST/%s", reservation->id);
	snprintf(vm_dir, sizeof(vm_dir), "/tmp/CloudIST/%s/%s", reservation->id, vm->id);
 
	
	// Criar pasta base /tmp/CloudIST e /tmp/CloudIST/<ID-reserva>
	mkdir("/tmp/CloudIST", 0755);
	mkdir(res_dir, 0755);
	// ja estava feito no copy_directory_recursive mkdir(vm_dir, 0755);
 
	char abs_input[MAX_PATH_SIZE];
	char abs_exec[MAX_PATH_SIZE];
	if (absolute_path(vm->type->input_folder, abs_input, sizeof(abs_input)) != 0 ||
	    absolute_path(vm->type->exec_path, abs_exec, sizeof(abs_exec)) != 0) {
		fprintf(stderr, "Nao foi possivel resolver os caminhos da vm %s\n", vm->id);
		_exit(EXIT_FAILURE);
	}
	// Copiar recursivamente
	int retorno=copy_directory_recursive(abs_input, vm_dir);
	if(retorno!=0){
		fprintf(stderr, "Copia da vm %s falhou", vm->id);
		_exit(1);
	}
 
	if (chdir(vm_dir) != 0) { 
		perror("chdir"); 
		_exit(EXIT_FAILURE); 
	}
	struct rlimit lim;
	lim.rlim_cur = lim.rlim_max = (rlim_t)vm->type->required.ram << 30;
	setrlimit(RLIMIT_AS, &lim);
 
	lim.rlim_cur = lim.rlim_max = (rlim_t)vm->type->required.disk << 30;
	setrlimit(RLIMIT_FSIZE, &lim);
 
	double pct = vm->type->required.cpu / vm->server->total.cpu * 100.0;
	char pct_str[32];
	snprintf(pct_str, sizeof(pct_str), "%d", (int)pct);   // cpulimit -l espera um inteiro
 
	execlp("cpulimit", "cpulimit", "-q", "-f", "-l", pct_str,
           "--", abs_exec, (char *)NULL);
 
	perror("execlp");
	_exit(EXIT_FAILURE);
	// fprintf(stderr, "VM execution not implemented in base version.\n");
	// return 0;
}

int spawn_all_vms( Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		VM *vm = res->vms[i];
		pid_t pid;
		switch (pid = fork()) {
		case -1: // error
			perror("fork");
			exit(EXIT_FAILURE);
 
		case 0: // child
			spawn_vm_child(vm, res);
			_exit(EXIT_FAILURE);
 
		default: // parent
			vm->pid = pid;
			vm->state = VM_STATE_RUNNING;
			break;
		}
	
		
 
		// TODO: Implement fork code. Set VM PID and update VM state to running.
 
	}
 
	return 0;
}

void wait_for_all_vms(Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		int status;
		waitpid(res->vms[i]->pid, &status, 0);
		res->vms[i]->state = VM_STATE_TERMINATED;
	}
}

const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}

void datacenter_reap_finished_vms(DataCenter *dc) {
	pthread_mutex_lock(&dc->reap_lock);
 
	VM *finished[MAX_RESERVATIONS * MAX_RESERVATION_VMS > 4096 ? 4096 : MAX_RESERVATIONS * MAX_RESERVATION_VMS];
	size_t nfinished = 0;
 
	/* Step 1: detect terminated processes (res_lock only). */
	pthread_mutex_lock(&dc->res_lock);
	for (size_t i = 0; i < dc->num_reservations; i++) {
		Reservation *res = &dc->reservations[i];
		for (size_t j = 0; j < res->num_vms; j++) {
			VM *vm = res->vms[j];
			if (vm->state != VM_STATE_RUNNING)
				continue;
			int status;
			if (waitpid(vm->pid, &status, WNOHANG) > 0) {
				vm->state = VM_STATE_TERMINATED;
				if (nfinished < sizeof(finished) / sizeof(finished[0]))
					finished[nfinished++] = vm;
			}
		}
	}
	pthread_mutex_unlock(&dc->res_lock);
 
	/* Step 2: give resources back, one server lock at a time. */
	for (size_t k = 0; k < nfinished; k++) {
		VM *vm = finished[k];
		Server *server = vm->server;
 
		pthread_mutex_lock(&server->lock);
		resources_add(&server->available, vm->type->required);
		for (size_t h = 0; h < server->num_hosted_vms; h++) {
			if (server->hosted_vms[h] == vm) {
				memmove(&server->hosted_vms[h], &server->hosted_vms[h + 1],
				        (server->num_hosted_vms - h - 1) * sizeof(VM *));
				server->num_hosted_vms--;
				break;
			}
		}
		pthread_mutex_unlock(&server->lock);
	}
 
	/* Step 3: delete reservations whose VMs have all terminated (and were released). */
	pthread_mutex_lock(&dc->res_lock);
	for (size_t i = 0; i < dc->num_reservations; ) {
		Reservation *res = &dc->reservations[i];
		int all_terminated = (res->num_vms > 0 && res->state == RES_STATE_RUNNING);
		for (size_t j = 0; all_terminated && j < res->num_vms; j++)
			if (res->vms[j]->state != VM_STATE_TERMINATED)
				all_terminated = 0;
 
		if (all_terminated) {
			for (size_t j = 0; j < res->num_vms; j++) {
				free(res->vms[j]);
				res->vms[j] = NULL;
			}
			res->num_vms = 0;
			memmove(&dc->reservations[i], &dc->reservations[i + 1],
			        (dc->num_reservations - i - 1) * sizeof(Reservation));
			dc->num_reservations--;
			continue;
		}
		i++;
	}
	pthread_mutex_unlock(&dc->res_lock);
 
	pthread_mutex_unlock(&dc->reap_lock);
}