#include <stdio.h>
#include <stdlib.h>

#include <psifiles.h>
#include <libciomr/libciomr.h>
#include <libpsio/psio.h>
#include <libchkpt/chkpt.h>
#include <libpsio/psio.hpp>
#include <libchkpt/chkpt.hpp>
#include <libiwl/iwl.h>
#include <libqt/qt.h>

#include <libmints/mints.h>
#include <boost/shared_ptr.hpp>
#include <libparallel/parallel.h>

#include "psi4-dec.h"

#include "deriv.h"

using namespace boost;

namespace psi { namespace deriv {

PsiReturnType deriv(Options & options)
{
    tstart();

    shared_ptr<PSIO> psio(new PSIO);
//    shared_ptr<Chkpt> chkpt(new Chkpt(psio, PSIO_OPEN_OLD));

    fprintf(outfile, " DERIV: Wrapper to libmints.\n   by Justin Turney\n\n");

    // We'll only be working with the active molecule.
    shared_ptr<Molecule> molecule = Process::environment.molecule();

    if (molecule.get() == 0) {
        fprintf(outfile, "  Active molecule not set!\n   Mints wrapper is not meant to be run with IPV1 inputs.");
        throw PSIEXCEPTION("Active molecule not set!");
    }

    // Create a new matrix factory
    shared_ptr<MatrixFactory> factory(new MatrixFactory);

    // Read in the basis set
    shared_ptr<BasisSetParser> parser(new Gaussian94BasisSetParser(options.get_str("BASIS_PATH")));
    shared_ptr<BasisSet> basisset = BasisSet::construct(parser, molecule, options.get_str("BASIS"));

    // Initialize an integral object.
    shared_ptr<IntegralFactory> integral(new IntegralFactory(basisset, basisset, basisset, basisset));

    // Create an SOBasisSet
    shared_ptr<SOBasisSet> sobasisset(new SOBasisSet(basisset, integral));
    SharedMatrix usotoao(sobasisset->petitelist()->sotoao());
    SharedMatrix aotoso(sobasisset->petitelist()->aotoso());

    const Dimension dimension = sobasisset->dimension();
    // Initialize the factory
    factory->init_with(dimension, dimension);

//    usotoao->print();
//    aotoso->print();

    // Print the molecule.
    basisset->molecule()->print();

    // Print out some useful information
    fprintf(outfile, "   Calculation information:\n");
    fprintf(outfile, "      Number of atoms:           %4d\n", molecule->natom());
    fprintf(outfile, "      Number of shells:          %4d\n", basisset->nshell());
    fprintf(outfile, "      Number of primitives:      %4d\n", basisset->nprimitive());
    fprintf(outfile, "      Number of atomic orbitals: %4d\n", basisset->nao());
    fprintf(outfile, "      Number of basis functions: %4d\n\n", basisset->nbf());

    // Form Q for RHF
    SharedSimpleMatrix Q(factory->create_simple_matrix("Q"));
    int *clsdpi = Process::environment.reference_wavefunction()->get_doccpi();

    // Read in C coefficients
    SharedMatrix Cso = Process::environment.reference_wavefunction()->get_Ca();
    SharedSimpleMatrix simple_Cso(new SimpleMatrix("Cso",
                                                   Process::environment.reference_wavefunction()->get_nso(),
                                                   Process::environment.reference_wavefunction()->get_nmo()));

    SharedSimpleMatrix simple_usotoao(new SimpleMatrix("USO -> AO",
                                                       Process::environment.reference_wavefunction()->get_nso(),
                                                       basisset->nbf()));
    SharedSimpleMatrix Cao(new SimpleMatrix("Cao",
                                            basisset->nbf(),
                                            Process::environment.reference_wavefunction()->get_nmo()));

    int sooffset = 0, mooffset = 0;
    for (int h=0; h<Cso->nirreps(); ++h) {
        for (int m=0; m<Cso->rowspi()[h]; ++m) {
            for (int n=0; n<basisset->nbf(); ++n) {
                simple_usotoao->set(sooffset+m, n, usotoao->get(h, m, n));
            }
            for (int n=0; n<Process::environment.reference_wavefunction()->get_nmopi()[h]; ++n) {
                simple_Cso->set(sooffset+m, n+mooffset, Cso->get(h, m, n));
            }
        }
        sooffset += Process::environment.reference_wavefunction()->get_nsopi()[h];
        mooffset += Cso->rowspi()[h];
    }

//    simple_usotoao->print();
//    simple_Cso->print();

    Cao->gemm(true, false, 1.0, simple_usotoao, simple_Cso, 0.0);
//    Cao->print();

    // Load in orbital energies
    SharedVector etmp = Process::environment.reference_wavefunction()->get_epsilon_a();
    shared_ptr<SimpleMatrix> W(factory->create_simple_matrix("W"));

    int nbf = basisset->nbf();
    for (int m=0; m<nbf; ++m) {
        for (int n=0; n<nbf; ++n) {
            double sum=0.0;
            double qsum=0.0;

            int mooffset = 0;
            for (int h=0; h<sobasisset->nirrep(); ++h) {
                for (int i=0; i<clsdpi[h]; ++i) {
                    sum += Cao->get(m, i+mooffset) * Cao->get(n, i+mooffset) * etmp->get(h, i);
                    qsum += Cao->get(m, i+mooffset) * Cao->get(n, i+mooffset);

//                    fprintf(outfile, "sum = %lf, qsum = %lf\n", sum, qsum);
                }
                mooffset += Process::environment.reference_wavefunction()->get_nmopi()[h];
            }
            W->set(m, n, sum);
            Q->set(m, n, qsum);
        }
    }

//    Q->print();
//    fprintf(outfile, "AO-basis\n");
//    W->print();

    SharedSimpleMatrix G;

    Deriv deriv(ref_rhf, factory, basisset);
    SharedSimpleMatrix WdS(deriv.overlap());
    SharedSimpleMatrix QdH(deriv.one_electron());
    SharedSimpleMatrix tb(deriv.two_body());
//    deriv.compute(Q, G, W);

    SimpleMatrix enuc = basisset->molecule()->nuclear_repulsion_energy_deriv1();

    enuc.print_atom_vector();
    QdH->print_atom_vector();
    WdS->print_atom_vector();
    tb->print_atom_vector();

    SimpleMatrix scf_grad("SCF gradient", basisset->molecule()->natom(), 3);
    scf_grad.add(&enuc);
    scf_grad.add(QdH);
    scf_grad.add(WdS);
    scf_grad.add(tb);

    scf_grad.print_atom_vector();

    GradientWriter grad(basisset->molecule(), scf_grad);
    grad.write("psi.file11.dat");

//    SimpleMatrix enuc2 = basis->molecule()->nuclear_repulsion_energy_deriv2();
//    enuc2.print();

    // Shut down psi
    tstop();

    return Success;
}

}}
