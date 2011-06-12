void comp(int i);
int foo(void);

void
test()
{
	int i;
	#pragma omp parallel for
	for (i=0; i<1<<20; i++){
		comp(i);
	}
}

void
test_nested_call()
{
	int i;
	#pragma omp parallel for
	for (i=0; i<1<<20; i++){
		test();
	}
}

void
test_nested()
{
	int i, j;
	#pragma omp parallel
	{
		#pragma omp for
		for (i=0; i<1<<20; i++){
			foo();
			#pragma omp parallel for
			for (j=0; j<1<<20; j++){
				test();
			}
		}
	}
}


void
test_double()
{
	int i;
	#pragma omp parallel
	{
		#pragma omp for
		for (i=0; i<1<<10; i++){
			comp(i);
		}

		#pragma omp for
		for (i=0; i<1<<10; i++){
			comp(i);
		}
	}
}
