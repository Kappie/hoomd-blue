#include "PPPMForceCompute.h"

using namespace boost::python;

bool is_pow2(unsigned int n)
    {
    while (n && n%2 == 0) { n/=2; }

    return (n == 1);
    };

//! Coefficients of a power expansion of sin(x)/x
const Scalar cpu_sinc_coeff[] = {Scalar(1.0), Scalar(-1.0/6.0), Scalar(1.0/120.0),
                        Scalar(-1.0/5040.0),Scalar(1.0/362880.0),
                        Scalar(-1.0/39916800.0)};

/*! \param sysdef The system definition
    \param nx Number of cells along first axis
    \param ny Number of cells along second axis
    \param nz Number of cells along third axis
    \param mode Per-type modes to multiply density
 */
PPPMForceCompute::PPPMForceCompute(boost::shared_ptr<SystemDefinition> sysdef,
    boost::shared_ptr<NeighborList> nlist,
    boost::shared_ptr<ParticleGroup> group)
    : ForceCompute(sysdef),
      m_nlist(nlist),
      m_group(group),
      m_n_ghost_cells(make_uint3(0,0,0)),
      m_grid_dim(make_uint3(0,0,0)),
      m_ghost_width(make_scalar3(0,0,0)),
      m_ghost_offset(0),
      m_n_cells(0),
      m_radius(1),
      m_n_inner_cells(0),
      m_need_initialize(true),
      m_params_set(false),
      m_box_changed(false),
      m_q(0.0),
      m_q2(0.0),
      m_kiss_fft_initialized(false),
      m_dfft_initialized(false)
    {

    m_boxchange_connection = m_pdata->connectBoxChange(boost::bind(&PPPMForceCompute::setBoxChange, this));
    // reset virial
    ArrayHandle<Scalar> h_virial(m_virial, access_location::host, access_mode::overwrite);
    memset(h_virial.data, 0, sizeof(Scalar)*m_virial.getNumElements());

    m_log_names.push_back("pppm_energy");

    m_mesh_points = make_uint3(0,0,0);
    m_global_dim = make_uint3(0,0,0);
    m_kappa = Scalar(0.0);
    m_rcut = Scalar(0.0);
    m_order = 0;
    }

void PPPMForceCompute::setParams(unsigned int nx, unsigned int ny, unsigned int nz,
    unsigned int order, Scalar kappa, Scalar rcut)
    {
    m_kappa = kappa;
    m_rcut = rcut;

    m_mesh_points = make_uint3(nx, ny, nz);
    m_global_dim = m_mesh_points;
    m_order = order;

    // radius for stencil and ghost cells
    m_radius = m_order/2;

    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        const Index3D& didx = m_pdata->getDomainDecomposition()->getDomainIndexer();

        if (!is_pow2(m_mesh_points.x) || !is_pow2(m_mesh_points.y) || !is_pow2(m_mesh_points.z))
            {
            m_exec_conf->msg->error()
                << "The number of mesh points along the every direction must be a power of two!" << std::endl;
            throw std::runtime_error("Error initializing charge.pppm");
            }

        if (nx % didx.getW())
            {
            m_exec_conf->msg->error()
                << "The number of mesh points along the x-direction ("<< nx <<") is not" << std::endl
                << "a multiple of the width (" << didx.getW() << ") of the processsor grid!" << std::endl
                << std::endl;
            throw std::runtime_error("Error initializing charge.pppm");
            }
        if (ny % didx.getH())
            {
            m_exec_conf->msg->error()
                << "The number of mesh points along the y-direction ("<< ny <<") is not" << std::endl 
                << "a multiple of the height (" << didx.getH() << ") of the processsor grid!" << std::endl
                << std::endl;
            throw std::runtime_error("Error initializing charge.pppm");
            }
        if (nz % didx.getD())
            {
            m_exec_conf->msg->error()
                << "The number of mesh points along the z-direction ("<< nz <<") is not" << std::endl
                << "a multiple of the depth (" << didx.getD() << ") of the processsor grid!" << std::endl
                << std::endl;
            throw std::runtime_error("Error initializing charge.pppm");
            }

        m_mesh_points.x /= didx.getW();
        m_mesh_points.y /= didx.getH();
        m_mesh_points.z /= didx.getD();
        }

    m_ghost_offset = 0;
    #endif // ENABLE_MPI

    GPUArray<Scalar> n_gf_b(order, m_exec_conf);
    m_gf_b.swap(n_gf_b);

    GPUArray<Scalar> n_rho_coeff(order*(2*order+1), m_exec_conf);
    m_rho_coeff.swap(n_rho_coeff);

    m_need_initialize = true;
    m_params_set = true;
    }

PPPMForceCompute::~PPPMForceCompute()
    {
    if (m_kiss_fft_initialized)
        {
        free(m_kiss_fft);
        free(m_kiss_ifft);
        kiss_fft_cleanup();
        }
    #ifdef ENABLE_MPI
    if (m_dfft_initialized)
        {
        dfft_destroy_plan(m_dfft_plan_forward);
        dfft_destroy_plan(m_dfft_plan_inverse);
        }
    #endif
    m_boxchange_connection.disconnect();
    }

//! Compute auxillary table for influence function
void PPPMForceCompute::compute_gf_denom()
    {
    unsigned int k,l,m;
    ArrayHandle<Scalar> h_gf_b(m_gf_b, access_location::host, access_mode::overwrite);
    for (l = 1; l < m_order; l++) h_gf_b.data[l] = 0.0;
    h_gf_b.data[0] = 1.0;

    for (m = 1; m < m_order; m++) {
        for (l = m; l > 0; l--) {
            h_gf_b.data[l] = 4.0 * (h_gf_b.data[l]*(l-m)*(l-m-0.5)-h_gf_b.data[l-1]*(l-m-1)*(l-m-1));
            }
        h_gf_b.data[0] = 4.0 * (h_gf_b.data[0]*(l-m)*(l-m-0.5));
    }

    int ifact = 1;
    for (k = 1; k < 2*m_order; k++) ifact *= k;
    Scalar gaminv = 1.0/ifact;
    for (l = 0; l < m_order; l++) h_gf_b.data[l] *= gaminv;
    }

//! Compute the denominator of the optimized influence function
Scalar PPPMForceCompute::gf_denom(Scalar x, Scalar y, Scalar z)
    {
    int l ;
    Scalar sx,sy,sz;
    ArrayHandle<Scalar> h_gf_b(m_gf_b, access_location::host, access_mode::read);
    sz = sy = sx = 0.0;
    for (l = m_order-1; l >= 0; l--) {
        sx = h_gf_b.data[l] + sx*x;
        sy = h_gf_b.data[l] + sy*y;
        sz = h_gf_b.data[l] + sz*z;
        }
    Scalar s = sx*sy*sz;
    return s*s;
    }

void PPPMForceCompute::compute_rho_coeff()
    {
    unsigned int j, k, l, m;
    Scalar s;
    Scalar a[136];
    ArrayHandle<Scalar> h_rho_coeff(m_rho_coeff, access_location::host, access_mode::readwrite);

    //    usage: a[x][y] = a[y + x*(2*m_order+1)]

    for(l=0; l<m_order; l++)
        {
        for(m=0; m<(2*m_order+1); m++)
            {
            a[m + l*(2*m_order +1)] = Scalar(0.0);
            }
        }

    for (k = -m_order; k <= m_order; k++)
        for (l = 0; l < m_order; l++) {
            a[(k+m_order) + l * (2*m_order+1)] = Scalar(0.0);
            }

    a[m_order + 0 * (2*m_order+1)] = Scalar(1.0);
    for (j = 1; j < m_order; j++) {
        for (k = -j; k <= j; k += 2) {
            s = 0.0;
            for (l = 0; l < j; l++) {
                a[(k + m_order) + (l+1)*(2*m_order+1)] = (a[(k+1+m_order) + l * (2*m_order + 1)] - a[(k-1+m_order) + l * (2*m_order + 1)]) / (l+1);
                s += pow(0.5,(double) (l+1)) * (a[(k-1+m_order) + l * (2*m_order + 1)] + pow(-1.0,(double) l) * a[(k+1+m_order) + l * (2*m_order + 1)] ) / (double)(l+1);
                }
            a[k+m_order + 0 * (2*m_order+1)] = s;
            }
        }

    m = 0;
    for (k = -(m_order-1); k < m_order; k += 2) {
        for (l = 0; l < m_order; l++) {
            h_rho_coeff.data[m + l*(2*m_order +1)] = a[k+m_order + l * (2*m_order + 1)];
            }
        m++;
        }
    }


Scalar PPPMForceCompute::rms(Scalar h, Scalar prd, Scalar natoms)
    {
    unsigned int m;
    Scalar sum = 0.0;
    Scalar acons[8][7];

    acons[1][0] = 2.0 / 3.0;
    acons[2][0] = 1.0 / 50.0;
    acons[2][1] = 5.0 / 294.0;
    acons[3][0] = 1.0 / 588.0;
    acons[3][1] = 7.0 / 1440.0;
    acons[3][2] = 21.0 / 3872.0;
    acons[4][0] = 1.0 / 4320.0;
    acons[4][1] = 3.0 / 1936.0;
    acons[4][2] = 7601.0 / 2271360.0;
    acons[4][3] = 143.0 / 28800.0;
    acons[5][0] = 1.0 / 23232.0;
    acons[5][1] = 7601.0 / 13628160.0;
    acons[5][2] = 143.0 / 69120.0;
    acons[5][3] = 517231.0 / 106536960.0;
    acons[5][4] = 106640677.0 / 11737571328.0;
    acons[6][0] = 691.0 / 68140800.0;
    acons[6][1] = 13.0 / 57600.0;
    acons[6][2] = 47021.0 / 35512320.0;
    acons[6][3] = 9694607.0 / 2095994880.0;
    acons[6][4] = 733191589.0 / 59609088000.0;
    acons[6][5] = 326190917.0 / 11700633600.0;
    acons[7][0] = 1.0 / 345600.0;
    acons[7][1] = 3617.0 / 35512320.0;
    acons[7][2] = 745739.0 / 838397952.0;
    acons[7][3] = 56399353.0 / 12773376000.0;
    acons[7][4] = 25091609.0 / 1560084480.0;
    acons[7][5] = 1755948832039.0 / 36229939200000.0;
    acons[7][6] = 4887769399.0 / 37838389248.0;

    for (m = 0; m < m_order; m++)
        sum += acons[m_order][m] * pow(h*m_kappa,Scalar(2.0)*(Scalar)m);
    Scalar value = m_q2 * pow(h*m_kappa,(Scalar)m_order) *
        sqrt(m_kappa*prd*sqrt(2.0*M_PI)*sum/natoms) / (prd*prd);
    return value;
    }

void PPPMForceCompute::setupCoeffs()
    {
    ArrayHandle<Scalar> h_charge(m_pdata->getCharges(), access_location::host, access_mode::read);

    // get system charge
    m_q = Scalar(0.0);
    m_q2 = Scalar(0.0);
    for(int i = 0; i < (int)m_pdata->getN(); i++) {
        m_q += h_charge.data[i];
        m_q2 += h_charge.data[i]*h_charge.data[i];
        }

    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        // reduce sum
        MPI_Allreduce(MPI_IN_PLACE,
                      &m_q,
                      1,
                      MPI_HOOMD_SCALAR,
                      MPI_SUM,
                      m_exec_conf->getMPICommunicator());
        MPI_Allreduce(MPI_IN_PLACE,
                      &m_q2,
                      1,
                      MPI_HOOMD_SCALAR,
                      MPI_SUM,
                      m_exec_conf->getMPICommunicator());
        }
    #endif

    if (fabs(m_q) > 1e-5)
        {
        m_exec_conf->msg->warning() << "charge.pppm: system in not neutral, the net charge is " << m_q << std::endl;
        }

    // compute RMS force error
    // NOTE: this is for an orthorhombic box, need to generalize to triclinic
    const BoxDim& global_box = m_pdata->getGlobalBox();
    Scalar3 L = global_box.getL();
    Scalar hx =  L.x/(Scalar)m_global_dim.x;
    Scalar hy =  L.y/(Scalar)m_global_dim.y;
    Scalar hz =  L.z/(Scalar)m_global_dim.z;
    Scalar lprx = PPPMForceCompute::rms(hx, L.x, (int)m_pdata->getNGlobal());
    Scalar lpry = PPPMForceCompute::rms(hy, L.y, (int)m_pdata->getNGlobal());
    Scalar lprz = PPPMForceCompute::rms(hz, L.z, (int)m_pdata->getNGlobal());
    Scalar lpr = sqrt(lprx*lprx + lpry*lpry + lprz*lprz) / sqrt(3.0);
    Scalar spr = 2.0*m_q2*exp(-m_kappa*m_kappa*m_rcut*m_rcut) / sqrt((int)m_pdata->getN()*m_rcut*L.x*L.y*L.z);

    double RMS_error = std::max(lpr,spr);
    if(RMS_error > 0.1) {
        m_exec_conf->msg->warning() << "charge.pppm: RMS error of " << RMS_error << " is probably too high! " << lpr << " " << spr << std::endl;
        }
    else{
        m_exec_conf->msg->notice(2) << "charge.pppm: RMS error: " << RMS_error << std::endl;
        }

    // initialize coefficients for charge assignment
    compute_rho_coeff();

    // initialize coefficients for Green's function
    compute_gf_denom();

    }
void PPPMForceCompute::setupMesh()
    {
    // update number of ghost cells
    m_n_ghost_cells = computeGhostCellNum();

    // extra ghost cells are as wide as the inner cells
    const BoxDim& box = m_pdata->getBox();
    Scalar3 cell_width = box.getNearestPlaneDistance() /
        make_scalar3(m_mesh_points.x, m_mesh_points.y, m_mesh_points.z);
    m_ghost_width = cell_width*make_scalar3( m_n_ghost_cells.x, m_n_ghost_cells.y, m_n_ghost_cells.z);

    m_exec_conf->msg->notice(6) << "charge.pppm: (Re-)allocating ghost layer ("
                                 << m_n_ghost_cells.x << ","
                                 << m_n_ghost_cells.y << ","
                                 << m_n_ghost_cells.z << ")" << std::endl;

    m_grid_dim = make_uint3(m_mesh_points.x+2*m_n_ghost_cells.x,
                           m_mesh_points.y+2*m_n_ghost_cells.y,
                           m_mesh_points.z+2*m_n_ghost_cells.z);

    m_n_cells = m_grid_dim.x*m_grid_dim.y*m_grid_dim.z;
    m_n_inner_cells = m_mesh_points.x * m_mesh_points.y * m_mesh_points.z;

    // allocate memory for influence function and k values
    GPUArray<Scalar> inf_f(m_n_inner_cells, m_exec_conf);
    m_inf_f.swap(inf_f);

    GPUArray<Scalar3> k(m_n_inner_cells, m_exec_conf);
    m_k.swap(k);

    GPUArray<Scalar> virial_mesh(6*m_n_inner_cells, m_exec_conf);
    m_virial_mesh.swap(virial_mesh);

    initializeFFT();
    }

uint3 PPPMForceCompute::computeGhostCellNum()
    {
    // ghost cells
    uint3 n_ghost_cells = make_uint3(0,0,0);
    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        Index3D di = m_pdata->getDomainDecomposition()->getDomainIndexer();
        n_ghost_cells.x = (di.getW() > 1) ? m_radius : 0;
        n_ghost_cells.y = (di.getH() > 1) ? m_radius : 0;
        n_ghost_cells.z = (di.getD() > 1) ? m_radius : 0;
        }
    #endif

    // extra ghost cells to accomodate skin layer (max 1/2 ghost layer width)
    #ifdef ENABLE_MPI
    if (m_comm)
        {
        Scalar r_buff = m_comm->getGhostLayerMaxWidth();

        const BoxDim& box = m_pdata->getBox();
        Scalar3 cell_width = box.getNearestPlaneDistance() /
            make_scalar3(m_mesh_points.x, m_mesh_points.y, m_mesh_points.z);

        if (n_ghost_cells.x) n_ghost_cells.x += r_buff/cell_width.x + 1;
        if (n_ghost_cells.y) n_ghost_cells.y += r_buff/cell_width.y + 1;
        if (n_ghost_cells.z) n_ghost_cells.z += r_buff/cell_width.z + 1;
        }
    #endif
    return n_ghost_cells;
    }

void PPPMForceCompute::initializeFFT()
    {
    bool local_fft = true;

    #ifdef ENABLE_MPI
    local_fft = !m_pdata->getDomainDecomposition();

    if (! local_fft)
        {
        // ghost cell communicator for charge interpolation
        m_grid_comm_forward = std::auto_ptr<CommunicatorGrid<kiss_fft_cpx> >(
            new CommunicatorGrid<kiss_fft_cpx>(m_sysdef,
               make_uint3(m_mesh_points.x, m_mesh_points.y, m_mesh_points.z),
               make_uint3(m_grid_dim.x, m_grid_dim.y, m_grid_dim.z),
               m_n_ghost_cells,
               true));
        // ghost cell communicator for force mesh
        m_grid_comm_reverse = std::auto_ptr<CommunicatorGrid<kiss_fft_cpx> >(
            new CommunicatorGrid<kiss_fft_cpx>(m_sysdef,
               make_uint3(m_mesh_points.x, m_mesh_points.y, m_mesh_points.z),
               make_uint3(m_grid_dim.x, m_grid_dim.y, m_grid_dim.z),
               m_n_ghost_cells,
               false));
        // set up distributed FFTs
        int gdim[3];
        int pdim[3];
        Index3D decomp_idx = m_pdata->getDomainDecomposition()->getDomainIndexer();
        pdim[0] = decomp_idx.getD();
        pdim[1] = decomp_idx.getH();
        pdim[2] = decomp_idx.getW();
        gdim[0] = m_mesh_points.z*pdim[0];
        gdim[1] = m_mesh_points.y*pdim[1];
        gdim[2] = m_mesh_points.x*pdim[2];
        int embed[3];
        embed[0] = m_mesh_points.z+2*m_n_ghost_cells.z;
        embed[1] = m_mesh_points.y+2*m_n_ghost_cells.y;
        embed[2] = m_mesh_points.x+2*m_n_ghost_cells.x;
        m_ghost_offset = (m_n_ghost_cells.z*embed[1]+m_n_ghost_cells.y)*embed[2]+m_n_ghost_cells.x;
        uint3 pcoord = m_pdata->getDomainDecomposition()->getGridPos();
        int pidx[3];
        pidx[0] = pcoord.z;
        pidx[1] = pcoord.y;
        pidx[2] = pcoord.x;
        int row_m = 0; /* both local grid and proc grid are row major, no transposition necessary */
        ArrayHandle<unsigned int> h_cart_ranks(m_pdata->getDomainDecomposition()->getCartRanks(),
            access_location::host, access_mode::read);
        dfft_create_plan(&m_dfft_plan_forward, 3, gdim, embed, NULL, pdim, pidx,
            row_m, 0, 1, m_exec_conf->getMPICommunicator(), (int *)h_cart_ranks.data);
        dfft_create_plan(&m_dfft_plan_inverse, 3, gdim, NULL, embed, pdim, pidx,
            row_m, 0, 1, m_exec_conf->getMPICommunicator(), (int *)h_cart_ranks.data);
        m_dfft_initialized = true;
        }
    #endif // ENABLE_MPI

    if (local_fft)
        {
        int dims[3];
        dims[0] = m_mesh_points.z;
        dims[1] = m_mesh_points.y;
        dims[2] = m_mesh_points.x;

        m_kiss_fft = kiss_fftnd_alloc(dims, 3, 0, NULL, NULL);
        m_kiss_ifft = kiss_fftnd_alloc(dims, 3, 1, NULL, NULL);

        m_kiss_fft_initialized = true;
        }

    // allocate mesh and transformed mesh
    GPUArray<kiss_fft_cpx> mesh(m_n_cells,m_exec_conf);
    m_mesh.swap(mesh);

    GPUArray<kiss_fft_cpx> fourier_mesh(m_n_inner_cells, m_exec_conf);
    m_fourier_mesh.swap(fourier_mesh);

    GPUArray<kiss_fft_cpx> fourier_mesh_G_x(m_n_inner_cells, m_exec_conf);
    m_fourier_mesh_G_x.swap(fourier_mesh_G_x);

    GPUArray<kiss_fft_cpx> fourier_mesh_G_y(m_n_inner_cells, m_exec_conf);
    m_fourier_mesh_G_y.swap(fourier_mesh_G_y);

    GPUArray<kiss_fft_cpx> fourier_mesh_G_z(m_n_inner_cells, m_exec_conf);
    m_fourier_mesh_G_z.swap(fourier_mesh_G_z);

    GPUArray<kiss_fft_cpx> inv_fourier_mesh_x(m_n_cells, m_exec_conf);
    m_inv_fourier_mesh_x.swap(inv_fourier_mesh_x);

    GPUArray<kiss_fft_cpx> inv_fourier_mesh_y(m_n_cells, m_exec_conf);
    m_inv_fourier_mesh_y.swap(inv_fourier_mesh_y);

    GPUArray<kiss_fft_cpx> inv_fourier_mesh_z(m_n_cells, m_exec_conf);
    m_inv_fourier_mesh_z.swap(inv_fourier_mesh_z);
    }

//! CPU implementation of sinc(x)==sin(x)/x
inline Scalar sinc(Scalar x)
    {
    Scalar sinc = 0;

    if (x*x <= Scalar(1.0))
        {
        Scalar term = Scalar(1.0);
        for (unsigned int i = 0; i < 6; ++i)
           {
           sinc += cpu_sinc_coeff[i] * term;
           term *= x*x;
           }
        }
    else
        {
        sinc = sin(x)/x;
        }

    return sinc;
    }

void PPPMForceCompute::computeInfluenceFunction()
    {
    if (m_prof) m_prof->push("influence function");

    ArrayHandle<Scalar> h_inf_f(m_inf_f,access_location::host, access_mode::overwrite);
    ArrayHandle<Scalar3> h_k(m_k,access_location::host, access_mode::overwrite);

    // reset arrays
    memset(h_inf_f.data, 0, sizeof(Scalar)*m_inf_f.getNumElements());
    memset(h_k.data, 0, sizeof(Scalar3)*m_k.getNumElements());

    const BoxDim& global_box = m_pdata->getGlobalBox();

    // compute reciprocal lattice vectors
    Scalar3 a1 = global_box.getLatticeVector(0);
    Scalar3 a2 = global_box.getLatticeVector(1);
    Scalar3 a3 = global_box.getLatticeVector(2);

    Scalar V_box = global_box.getVolume();
    Scalar3 b1 = Scalar(2.0*M_PI)*make_scalar3(a2.y*a3.z-a2.z*a3.y, a2.z*a3.x-a2.x*a3.z, a2.x*a3.y-a2.y*a3.x)/V_box;
    Scalar3 b2 = Scalar(2.0*M_PI)*make_scalar3(a3.y*a1.z-a3.z*a1.y, a3.z*a1.x-a3.x*a1.z, a3.x*a1.y-a3.y*a1.x)/V_box;
    Scalar3 b3 = Scalar(2.0*M_PI)*make_scalar3(a1.y*a2.z-a1.z*a2.y, a1.z*a2.x-a1.x*a2.z, a1.x*a2.y-a1.y*a2.x)/V_box;

    bool local_fft = m_kiss_fft_initialized;

    #ifdef ENABLE_MPI
    uint3 pdim=make_uint3(0,0,0);
    uint3 pidx=make_uint3(0,0,0);
    if (m_pdata->getDomainDecomposition())
        {
        const Index3D &didx = m_pdata->getDomainDecomposition()->getDomainIndexer();
        pidx = m_pdata->getDomainDecomposition()->getGridPos();
        pdim = make_uint3(didx.getW(), didx.getH(), didx.getD());
        }
    #endif

    Scalar3 kH = Scalar(2.0*M_PI)*make_scalar3(Scalar(1.0)/(Scalar)m_global_dim.x,
                                               Scalar(1.0)/(Scalar)m_global_dim.y,
                                               Scalar(1.0)/(Scalar)m_global_dim.z);

    for (unsigned int cell_idx = 0; cell_idx < m_n_inner_cells; ++cell_idx)
        {
        uint3 wave_idx;
        #ifdef ENABLE_MPI
        if (! local_fft)
           {
           // local layout: row major
           int ny = m_mesh_points.y;
           int nx = m_mesh_points.x;
           int n_local = cell_idx/ny/nx;
           int m_local = (cell_idx-n_local*ny*nx)/nx;
           int l_local = cell_idx % nx;
           // cyclic distribution
           wave_idx.x = l_local*pdim.x + pidx.x;
           wave_idx.y = m_local*pdim.y + pidx.y;
           wave_idx.z = n_local*pdim.z + pidx.z;
           }
        else
        #endif
            {
            // kiss FFT expects data in row major format
            wave_idx.z = cell_idx / (m_mesh_points.y * m_mesh_points.x);
            wave_idx.y = (cell_idx - wave_idx.z * m_mesh_points.x * m_mesh_points.y)/ m_mesh_points.x;
            wave_idx.x = cell_idx % m_mesh_points.x;
            }

        int3 n = make_int3(wave_idx.x,wave_idx.y,wave_idx.z);

        // compute Miller indices
        if (n.x >= (int)(m_global_dim.x/2 + m_global_dim.x%2))
            n.x -= (int) m_global_dim.x;
        if (n.y >= (int)(m_global_dim.y/2 + m_global_dim.y%2))
            n.y -= (int) m_global_dim.y;
        if (n.z >= (int)(m_global_dim.z/2 + m_global_dim.z%2))
            n.z -= (int) m_global_dim.z;

        Scalar3 k = (Scalar)n.x*b1+(Scalar)n.y*b2+(Scalar)n.z*b3;

        Scalar snx = fast::sin(0.5*kH.x*(Scalar)n.x);
        Scalar sny = fast::sin(0.5*kH.y*(Scalar)n.y);
        Scalar snz = fast::sin(0.5*kH.z*(Scalar)n.z);

        if (n.x != 0 && n.y != 0 && n.z != 0)
            {
            Scalar sum1(0.0);
            Scalar numerator = Scalar(4.0*M_PI);

            Scalar denominator = gf_denom(snx*snx, sny*sny, snz*snz);

            for (int ix = -(int)m_global_dim.x/2; ix < (int)m_global_dim.x; ix++)
                {
                Scalar qx = (n.x + (Scalar)ix*m_global_dim.x);
                Scalar3 knx = (Scalar)qx*b1;

                Scalar argx = Scalar(0.5)*kH.x;
                Scalar wxs = sinc(argx);
                Scalar wx(1.0);
                for (unsigned int iorder = 0; iorder < m_order; ++iorder)
                    {
                    wx *= wxs;
                    }

                for (int iy = -(int)m_global_dim.y/2; iy < (int)m_global_dim.y; iy++)
                    {
                    Scalar qy = (n.y + (Scalar)iy*m_global_dim.y);
                    Scalar3 kny = (Scalar)qy*b2;

                    Scalar argy = Scalar(0.5)*kH.y;
                    Scalar wys = sinc(argy);
                    Scalar wy(1.0);
                    for (unsigned int iorder = 0; iorder < m_order; ++iorder)
                        {
                        wy *= wys;
                        }

                    for (int iz = -(int)m_global_dim.z/2; iz < (int)m_global_dim.z; iz++)
                        {
                        Scalar qz = (n.z + (Scalar)iz*m_global_dim.z);
                        Scalar3 knz = (Scalar)qz*b3;

                        Scalar argz = Scalar(0.5)*kH.z;
                        Scalar wzs = sinc(argz);
                        Scalar wz(1.0);
                        for (unsigned int iorder = 0; iorder < m_order; ++iorder)
                            {
                            wz *= wzs;
                            }

                        Scalar3 kn = knx + kny + knz;
                        Scalar dot1 = dot(kn, k);
                        Scalar dot2 = dot(kn, kn);

                        Scalar arg_gauss = Scalar(0.25)*dot2/m_kappa/m_kappa;
                        Scalar gauss = exp(-arg_gauss);

                        sum1 += (dot1/dot2) * gauss * wx * wx * wy * wy * wz * wz;
                        }
                    }
                }
            h_inf_f.data[cell_idx] = numerator*sum1/denominator;
            }
        else // q=0
            {
            h_inf_f.data[cell_idx] = Scalar(0.0);
            }

        h_k.data[cell_idx] = k;
        }

    if (m_prof) m_prof->pop();
    }

//! Assignment of particles to mesh using variable order interpolation scheme
void PPPMForceCompute::assignParticles()
    {
    if (m_prof) m_prof->push("assign");

    ArrayHandle<Scalar4> h_postype(m_pdata->getPositions(), access_location::host, access_mode::read);
    ArrayHandle<kiss_fft_cpx> h_mesh(m_mesh, access_location::host, access_mode::overwrite);
    ArrayHandle<Scalar> h_charge(m_pdata->getCharges(), access_location::host, access_mode::read);

    ArrayHandle<Scalar> h_rho_coeff(m_rho_coeff,access_location::host, access_mode::read);

    const BoxDim& box = m_pdata->getBox();

    // set mesh to zero
    memset(h_mesh.data, 0, sizeof(kiss_fft_cpx)*m_mesh.getNumElements());

    unsigned int nparticles = m_pdata->getN();

    // loop over local particles
    for (unsigned int idx = 0; idx < nparticles; ++idx)
        {
        Scalar4 postype = h_postype.data[idx];

        Scalar3 pos = make_scalar3(postype.x, postype.y, postype.z);

        Scalar qi = h_charge.data[idx];

        // compute coordinates in units of the mesh size
        Scalar3 f = box.makeFraction(pos);
        Scalar3 reduced_pos = make_scalar3(f.x * (Scalar) m_mesh_points.x,
                                           f.y * (Scalar) m_mesh_points.y,
                                           f.z * (Scalar) m_mesh_points.z);

        Scalar shift, shiftone;

        if (m_order % 2)
            {
            shift =0.5;
            shiftone = 0.0;
            }
        else
            {
            shift = 0.0;
            shiftone = 0.5;
            }

        // find cell of the mesh the particle is in
        unsigned int ix = (reduced_pos.x + (Scalar)m_n_ghost_cells.x) + shift;
        unsigned int iy = (reduced_pos.y + (Scalar)m_n_ghost_cells.y) + shift;
        unsigned int iz = (reduced_pos.z + (Scalar)m_n_ghost_cells.z) + shift;

        // handle particles on the boundary
        if (ix == m_grid_dim.x && !m_n_ghost_cells.x)
            ix = 0;
        if (iy == m_grid_dim.y && !m_n_ghost_cells.y)
            iy = 0;
        if (iz == m_grid_dim.z && !m_n_ghost_cells.z)
            iz = 0;

        if (ix < 0 || ix >= m_grid_dim.x ||
            iy < 0 || iy >= m_grid_dim.y ||
            iz < 0 || iz >= m_grid_dim.z)
            {
            // ignore, error will be thrown elsewhere (in CellList)
            continue;
            }

        Scalar dx = shiftone+(Scalar)ix-reduced_pos.x;
        Scalar dy = shiftone+(Scalar)iy-reduced_pos.y;
        Scalar dz = shiftone+(Scalar)iz-reduced_pos.z;

        int mult_fact = 2*m_order+1;
        Scalar Wx, Wy, Wz;

        for (int i = -m_radius; i <= (int)m_radius ; ++i)
            {
            Wx = Scalar(0.0);
            for (int iorder = m_order-1; iorder >= 0; iorder--)
                {
                Wx = h_rho_coeff.data[i + m_radius + iorder*mult_fact] + Wx * dx;
                }

            int neighi = (int)ix + i;

            if (! m_n_ghost_cells.x)
                {
                if (neighi == (int)m_grid_dim.x)
                    neighi = 0;
                else if (neighi < 0)
                    neighi += m_grid_dim.x;
                }


            for (int j = -m_radius; j <= (int)m_radius; ++j)
                {
                Wy = Scalar(0.0);
                for (int iorder = m_order-1; iorder >= 0; iorder--)
                    {
                    Wy = h_rho_coeff.data[j + m_radius + iorder*mult_fact] + Wy * dy;
                    }

                int neighj = (int)iy + j;

                if (! m_n_ghost_cells.y)
                    {
                    if (neighj == (int)m_grid_dim.y)
                        neighj = 0;
                    else if (neighj < 0)
                        neighj += m_grid_dim.y;
                    }


                for (int k = -m_radius; k <= (int)m_radius; ++k)
                    {
                    Wz = Scalar(0.0);
                    for (int iorder = m_order-1; iorder >= 0; iorder--)
                        {
                        Wz = h_rho_coeff.data[k + m_radius + iorder*mult_fact] + Wz * dz;
                        }

                    int neighk = (int)iz + k;
                    if (! m_n_ghost_cells.z)
                        {
                        if (neighk == (int)m_grid_dim.z)
                            neighk = 0;
                        else if (neighk < 0)
                            neighk += m_grid_dim.z;
                        }

                    Scalar W = Wx*Wy*Wz;

                    // store in row major order
                    unsigned int neigh_idx = neighi + m_grid_dim.x * (neighj + m_grid_dim.y*neighk);

                    h_mesh.data[neigh_idx].r += qi*W;
                    }
                }
            }
        } // end loop over particles

    if (m_prof) m_prof->pop();
    }

void PPPMForceCompute::updateMeshes()
    {

    ArrayHandle<Scalar> h_inf_f(m_inf_f, access_location::host, access_mode::read);
    ArrayHandle<Scalar3> h_k(m_k, access_location::host, access_mode::read);

    if (m_kiss_fft_initialized)
        {
        if (m_prof) m_prof->push("FFT");
        // transform the particle mesh locally (forward transform)
        ArrayHandle<kiss_fft_cpx> h_mesh(m_mesh, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh(m_fourier_mesh, access_location::host, access_mode::overwrite);

        kiss_fftnd(m_kiss_fft, h_mesh.data, h_fourier_mesh.data);
        if (m_prof) m_prof->pop();
        }

    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        // update inner cells of particle mesh
        if (m_prof) m_prof->push("ghost cell update");
        m_exec_conf->msg->notice(8) << "charge.pppm: Ghost cell update" << std::endl;
        m_grid_comm_forward->communicate(m_mesh);
        if (m_prof) m_prof->pop();

        // perform a distributed FFT
        m_exec_conf->msg->notice(8) << "charge.pppm: Distributed FFT mesh" << std::endl;

        if (m_prof) m_prof->push("FFT");
        ArrayHandle<kiss_fft_cpx> h_mesh(m_mesh, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh(m_fourier_mesh, access_location::host, access_mode::overwrite);

        dfft_execute((cpx_t *)(h_mesh.data+m_ghost_offset), (cpx_t *)h_fourier_mesh.data, 0,m_dfft_plan_forward);
        if (m_prof) m_prof->pop();
        }
    #endif

    ArrayHandle<kiss_fft_cpx> h_fourier_mesh(m_fourier_mesh, access_location::host, access_mode::readwrite);

        {
        ArrayHandle<Scalar3> h_k(m_k, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_x(m_fourier_mesh_G_x, access_location::host, access_mode::overwrite);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_y(m_fourier_mesh_G_y, access_location::host, access_mode::overwrite);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_z(m_fourier_mesh_G_z, access_location::host, access_mode::overwrite);

        unsigned int NNN = m_mesh_points.x*m_mesh_points.y*m_mesh_points.z;

        #ifdef ENABLE_MPI
        if (m_pdata->getDomainDecomposition())
            {
            Index3D di = m_pdata->getDomainDecomposition()->getDomainIndexer();
            NNN *= di.getW()*di.getH()*di.getD();
            }
        #endif

        // multiply with influence function and k vector
        for (unsigned int k = 0; k < m_n_inner_cells; ++k)
            {
            kiss_fft_cpx f = h_fourier_mesh.data[k];

            Scalar scaled_inf_f = h_inf_f.data[k] / (Scalar)NNN;

            Scalar3 kvec = h_k.data[k];

            h_fourier_mesh_G_x.data[k].r = f.r * kvec.x * scaled_inf_f;
            h_fourier_mesh_G_x.data[k].i = f.i * kvec.x * scaled_inf_f;

            h_fourier_mesh_G_y.data[k].r = f.r * kvec.y * scaled_inf_f;
            h_fourier_mesh_G_y.data[k].i = f.i * kvec.y * scaled_inf_f;

            h_fourier_mesh_G_z.data[k].r = f.r * kvec.z * scaled_inf_f;
            h_fourier_mesh_G_z.data[k].i = f.i * kvec.z * scaled_inf_f;

            h_fourier_mesh.data[k] = f;
            }
        }

    if (m_prof) m_prof->pop();

    if (m_kiss_fft_initialized)
        {
        if (m_prof) m_prof->push("FFT");
        // do a local inverse transform of the force mesh
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_x(m_fourier_mesh_G_x, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_y(m_fourier_mesh_G_y, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_z(m_fourier_mesh_G_z, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_x(m_inv_fourier_mesh_x, access_location::host, access_mode::overwrite);
        ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_y(m_inv_fourier_mesh_y, access_location::host, access_mode::overwrite);
        ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_z(m_inv_fourier_mesh_z, access_location::host, access_mode::overwrite);
        kiss_fftnd(m_kiss_ifft, h_fourier_mesh_G_x.data, h_inv_fourier_mesh_x.data);
        kiss_fftnd(m_kiss_ifft, h_fourier_mesh_G_y.data, h_inv_fourier_mesh_y.data);
        kiss_fftnd(m_kiss_ifft, h_fourier_mesh_G_z.data, h_inv_fourier_mesh_z.data);
        if (m_prof) m_prof->pop();
        }

    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        if (m_prof) m_prof->push("FFT");
        // Distributed inverse transform force on mesh points
        m_exec_conf->msg->notice(8) << "charge.pppm: Distributed iFFT" << std::endl;

        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_x(m_fourier_mesh_G_x, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_y(m_fourier_mesh_G_y, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_fourier_mesh_G_z(m_fourier_mesh_G_z, access_location::host, access_mode::read);
        ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_x(m_inv_fourier_mesh_x, access_location::host, access_mode::overwrite);
        ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_y(m_inv_fourier_mesh_y, access_location::host, access_mode::overwrite);
        ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_z(m_inv_fourier_mesh_z, access_location::host, access_mode::overwrite);

        dfft_execute((cpx_t *)h_fourier_mesh_G_x.data, (cpx_t *)(h_inv_fourier_mesh_x.data+m_ghost_offset), 1,m_dfft_plan_inverse);
        dfft_execute((cpx_t *)h_fourier_mesh_G_y.data, (cpx_t *)(h_inv_fourier_mesh_y.data+m_ghost_offset), 1,m_dfft_plan_inverse);
        dfft_execute((cpx_t *)h_fourier_mesh_G_z.data, (cpx_t *)(h_inv_fourier_mesh_z.data+m_ghost_offset), 1,m_dfft_plan_inverse);
        if (m_prof) m_prof->pop();
        }
    #endif

    // potential optimization: combine vector components into Scalar3

    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        // update outer cells of force mesh using ghost cells from neighboring processors
        if (m_prof) m_prof->push("ghost cell update");
        m_exec_conf->msg->notice(8) << "charge.pppm: Ghost cell update" << std::endl;
        m_grid_comm_reverse->communicate(m_inv_fourier_mesh_x);
        m_grid_comm_reverse->communicate(m_inv_fourier_mesh_y);
        m_grid_comm_reverse->communicate(m_inv_fourier_mesh_z);
        if (m_prof) m_prof->pop();
        }
    #endif
    }

void PPPMForceCompute::interpolateForces()
    {
    if (m_prof) m_prof->push("interpolate");

    // access particle data
    ArrayHandle<Scalar4> h_postype(m_pdata->getPositions(), access_location::host, access_mode::read);
    ArrayHandle<Scalar> h_charge(m_pdata->getCharges(), access_location::host, access_mode::read);

    // access inverse Fourier tranform mesh
    ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_x(m_inv_fourier_mesh_x, access_location::host, access_mode::read);
    ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_y(m_inv_fourier_mesh_y, access_location::host, access_mode::read);
    ArrayHandle<kiss_fft_cpx> h_inv_fourier_mesh_z(m_inv_fourier_mesh_z, access_location::host, access_mode::read);

    // access force array
    ArrayHandle<Scalar4> h_force(m_force, access_location::host, access_mode::overwrite);

    ArrayHandle<Scalar> h_rho_coeff(m_rho_coeff, access_location::host, access_mode::read);

    const BoxDim& box = m_pdata->getBox();

    unsigned int nptl = m_pdata->getN();
    for (unsigned int idx = 0; idx < nptl; ++idx)
        {
        Scalar4 postype = h_postype.data[idx];

        Scalar3 pos = make_scalar3(postype.x, postype.y, postype.z);

        Scalar qi = h_charge.data[idx];

        // compute coordinates in units of the mesh size
        Scalar3 f = box.makeFraction(pos);
        Scalar3 reduced_pos = make_scalar3(f.x * (Scalar) m_mesh_points.x,
                                           f.y * (Scalar) m_mesh_points.y,
                                           f.z * (Scalar) m_mesh_points.z);

        Scalar shift, shiftone;

        if (m_order % 2)
            {
            shift =0.5;
            shiftone = 0.0;
            }
        else
            {
            shift = 0.0;
            shiftone = 0.5;
            }


        // find cell of the force mesh the particle is in
        unsigned int ix = (reduced_pos.x + (Scalar)m_n_ghost_cells.x) + shift;
        unsigned int iy = (reduced_pos.y + (Scalar)m_n_ghost_cells.y) + shift;
        unsigned int iz = (reduced_pos.z + (Scalar)m_n_ghost_cells.z) + shift;

        // handle particles on the boundary
        if (ix == m_grid_dim.x && !m_n_ghost_cells.x)
            ix = 0;
        if (iy == m_grid_dim.y && !m_n_ghost_cells.y)
            iy = 0;
        if (iz == m_grid_dim.z && !m_n_ghost_cells.z)
            iz = 0;

        if (ix < 0 || ix >= m_grid_dim.x ||
            iy < 0 || iy >= m_grid_dim.y ||
            iz < 0 || iz >= m_grid_dim.z)
            {
            // ignore, error will be thrown elsewhere (in CellList)
            continue;
            }

        Scalar3 force = make_scalar3(0.0,0.0,0.0);

        Scalar dx = shiftone+(Scalar)ix-reduced_pos.x;
        Scalar dy = shiftone+(Scalar)iy-reduced_pos.y;
        Scalar dz = shiftone+(Scalar)iz-reduced_pos.z;

        int mult_fact = 2*m_order+1;
        Scalar Wx, Wy, Wz;

        for (int i = -m_radius; i <= (int)m_radius ; ++i)
            {
            Wx = Scalar(0.0);
            for (int iorder = m_order-1; iorder >= 0; iorder--)
                {
                Wx = h_rho_coeff.data[i + m_radius + iorder*mult_fact] + Wx * dx;
                }

            int neighi = (int)ix + i;

            if (! m_n_ghost_cells.x)
                {
                if (neighi == (int)m_grid_dim.x)
                    neighi = 0;
                else if (neighi < 0)
                    neighi += m_grid_dim.x;
                }


            for (int j = -m_radius; j <= (int)m_radius; ++j)
                {
                Wy = Scalar(0.0);
                for (int iorder = m_order-1; iorder >= 0; iorder--)
                    {
                    Wy = h_rho_coeff.data[j + m_radius + iorder*mult_fact] + Wy * dy;
                    }

                int neighj = (int)iy + j;

                if (! m_n_ghost_cells.y)
                    {
                    if (neighj == (int)m_grid_dim.y)
                        neighj = 0;
                    else if (neighj < 0)
                        neighj += m_grid_dim.y;
                    }


                for (int k = -m_radius; k <= (int)m_radius; ++k)
                    {
                    Wz = Scalar(0.0);
                    for (int iorder = m_order-1; iorder >= 0; iorder--)
                        {
                        Wz = h_rho_coeff.data[k + m_radius + iorder*mult_fact] + Wz * dz;
                        }

                    int neighk = (int)iz + k;
                    if (! m_n_ghost_cells.z)
                        {
                        if (neighk == (int)m_grid_dim.z)
                            neighk = 0;
                        else if (neighk < 0)
                            neighk += m_grid_dim.z;
                        }

                    unsigned int neigh_idx;
                    neigh_idx = neighi + m_grid_dim.x * (neighj + m_grid_dim.y*neighk);

                    kiss_fft_cpx E_x = h_inv_fourier_mesh_x.data[neigh_idx];
                    kiss_fft_cpx E_y = h_inv_fourier_mesh_y.data[neigh_idx];
                    kiss_fft_cpx E_z = h_inv_fourier_mesh_z.data[neigh_idx];

                    Scalar W = Wx * Wy * Wz;
                    force.x += qi*W*E_x.r;
                    force.y += qi*W*E_y.r;
                    force.z += qi*W*E_z.r;
                    }
                }
            }

        h_force.data[idx] = make_scalar4(force.x,force.y,force.z,0.0);
        }  // end of loop over particles

    if (m_prof) m_prof->pop();
    }

Scalar PPPMForceCompute::computePE()
    {
    if (m_prof) m_prof->push("sum");

    ArrayHandle<kiss_fft_cpx> h_fourier_mesh(m_fourier_mesh, access_location::host, access_mode::read);
    ArrayHandle<Scalar> h_inf_f(m_inf_f, access_location::host, access_mode::read);

    Scalar sum(0.0);

    bool exclude_dc = true;
    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        uint3 my_pos = m_pdata->getDomainDecomposition()->getGridPos();
        exclude_dc = !my_pos.x && !my_pos.y && !my_pos.z;
        }
    #endif

    for (unsigned int k = 0; k < m_n_inner_cells; ++k)
        {
        bool exclude = false;
        if (exclude_dc)
            // exclude DC bin
            exclude = (k == 0);

        if (! exclude)
            {
            sum += (h_fourier_mesh.data[k].r * h_fourier_mesh.data[k].r
                + h_fourier_mesh.data[k].i * h_fourier_mesh.data[k].i)*h_inf_f.data[k];
            }
        }

    sum *= Scalar(1.0/2.0);

    if (m_prof) m_prof->pop();

    Scalar V = m_pdata->getGlobalBox().getVolume();
    Scalar scale = Scalar(1.0)/((Scalar)(m_global_dim.x*m_global_dim.y*m_global_dim.z));
    sum *= Scalar(0.5)*V*scale*scale;

    if (m_exec_conf->getRank()==0)
        {
        // add correction on rank 0
        sum -= m_q2 * m_kappa / Scalar(1.772453850905516027298168);
        sum -= Scalar(0.5*M_PI)*m_q*m_q / (m_kappa*m_kappa* V);
        }

    // store this rank's contribution as external potential energy
    m_external_energy = sum;

    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        // reduce sum
        MPI_Allreduce(MPI_IN_PLACE,
                      &sum,
                      1,
                      MPI_HOOMD_SCALAR,
                      MPI_SUM,
                      m_exec_conf->getMPICommunicator());
        }
    #endif

    return sum;
    }

void PPPMForceCompute::computeForces(unsigned int timestep)
    {
    if (m_prof) m_prof->push("PPPM");

    if (m_need_initialize)
        {
        if (!m_params_set)
            {
            m_exec_conf->msg->error() << "charge.pppm: charge.pppm() requires parameters to be set before run()"
                << std::endl;
            throw std::runtime_error("Error computing PPPM forces");
            }

        // allocate memory and initialize arrays
        setupMesh();

        // setup tables and do misc validation
        setupCoeffs();

        computeInfluenceFunction();
        m_need_initialize = false;
        }

    bool ghost_cell_num_changed = false;
    uint3 n_ghost_cells = computeGhostCellNum();

    // do we need to reallocate?
    if (m_n_ghost_cells.x != n_ghost_cells.x ||
        m_n_ghost_cells.y != n_ghost_cells.y ||
        m_n_ghost_cells.z != n_ghost_cells.z)
        ghost_cell_num_changed = true;

    if (m_box_changed || ghost_cell_num_changed)
        {
        if (ghost_cell_num_changed) setupMesh();
        computeInfluenceFunction();
        m_box_changed = false;
        }

    assignParticles();

    updateMeshes();

    PDataFlags flags = this->m_pdata->getFlags();
    if (flags[pdata_flag::potential_energy])
        {
        computePE();
        }

    interpolateForces();

    if (flags[pdata_flag::pressure_tensor] || flags[pdata_flag::isotropic_virial])
        {
        computeVirial();
        }
    else
        {
        for (unsigned int i = 0; i < 6; ++i)
            m_external_virial[i] = Scalar(0.0);
        }

    if (m_prof) m_prof->pop();
    }

void PPPMForceCompute::computeVirial()
    {
    if (m_prof) m_prof->push("virial");

    ArrayHandle<kiss_fft_cpx> h_fourier_mesh(m_fourier_mesh, access_location::host, access_mode::overwrite);

    ArrayHandle<Scalar> h_inf_f(m_inf_f, access_location::host, access_mode::read);
    ArrayHandle<Scalar3> h_k(m_k, access_location::host, access_mode::read);


    Scalar virial[6];
    for (unsigned int i = 0; i < 6; ++i)
        virial[i] = Scalar(0.0);

    bool exclude_dc = true;
    #ifdef ENABLE_MPI
    if (m_pdata->getDomainDecomposition())
        {
        uint3 my_pos = m_pdata->getDomainDecomposition()->getGridPos();
        exclude_dc = !my_pos.x && !my_pos.y && !my_pos.z;
        }
    #endif

    for (unsigned int kidx = 0; kidx < m_n_inner_cells; ++kidx)
        {
        bool exclude = false;
        if (exclude_dc)
            // exclude DC bin
            exclude = (kidx == 0);

        if (! exclude)
            {
            // non-zero wave vector
            kiss_fft_cpx fourier = h_fourier_mesh.data[kidx];

            Scalar3 k = h_k.data[kidx];
            Scalar ksq = dot(k,k);

            Scalar rhog = (fourier.r * fourier.r + fourier.i * fourier.i)*h_inf_f.data[kidx];

            Scalar vterm = -Scalar(2.0)*(Scalar(1.0)/ksq + Scalar(0.25)/(m_kappa*m_kappa));
            virial[0] += rhog*(Scalar(1.0) + vterm*k.x*k.x); // xx
            virial[1] += rhog*(              vterm*k.x*k.y); // xy
            virial[2] += rhog*(              vterm*k.x*k.z); // xz
            virial[3] += rhog*(Scalar(1.0) + vterm*k.y*k.y); // yy
            virial[4] += rhog*(              vterm*k.y*k.z); // yz
            virial[5] += rhog*(Scalar(1.0) + vterm*k.z*k.z); // zz
            }
        }

    Scalar V = m_pdata->getGlobalBox().getVolume();
    Scalar scale = Scalar(1.0)/((Scalar)(m_global_dim.x*m_global_dim.y*m_global_dim.z));

    for (unsigned int k = 0; k < 6; ++k)
        {
        // store this rank's contribution in m_external_virial
        m_external_virial[k] = virial[k]*V*scale*scale;
        }

    if (m_prof) m_prof->pop();
    }

Scalar PPPMForceCompute::getLogValue(const std::string& quantity, unsigned int timestep)
    {
    if (quantity == m_log_names[0])
        {
        return computePE();
        }

    // nothing found? return base class value
    return ForceCompute::getLogValue(quantity, timestep);
    }

void export_PPPMForceCompute()
    {
    class_<PPPMForceCompute, boost::shared_ptr<PPPMForceCompute>, bases<ForceCompute>, boost::noncopyable >
        ("PPPMForceCompute", init< boost::shared_ptr<SystemDefinition>,
            boost::shared_ptr<NeighborList>, boost::shared_ptr<ParticleGroup> >())
        .def("setParams", &PPPMForceCompute::setParams)
        ;
    }
