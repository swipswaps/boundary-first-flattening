#include "Bff.h"
#include "Distortion.h"
#include <cmath>

namespace bff {

BFF::BFF(Mesh& mesh_):
mesh(mesh_),
inputSurfaceData(std::shared_ptr<BFFData>(new BFFData(mesh))),
cutSurfaceData(NULL)
{
	data = inputSurfaceData;
}

void BFF::processCut(const DenseMatrix& u, DenseMatrix& a, DenseMatrix& g)
{
	// set the current data to the data of the cut surface
	data = cutSurfaceData;

	// copy scale factors u of the original surface into a and g that store the interior
	// and boundary scale factors u (resp.) of the cut surface
	a = DenseMatrix(data->iN);
	g = DenseMatrix(data->bN);
	for (WedgeCIter w = mesh.wedges().begin(); w != mesh.wedges().end(); w++) {
		if (w->vertex()->onBoundary()) g(data->bIndex[w]) = u(inputSurfaceData->index[w]);
		else a(data->index[w]) = u(inputSurfaceData->index[w]);
	}
}

void BFF::computeBoundaryScaleFactors(const DenseMatrix& ltilde, DenseMatrix& u) const
{
	u = DenseMatrix(data->bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];
		int j = data->bIndex[w->prev()];

		// compute u as a weighted average of piecewise constant scale factors per
		// boundary edge
		double uij = log(ltilde(i)/data->l(i));
		double ujk = log(ltilde(j)/data->l(j));
		u(j) = (ltilde(i)*uij + ltilde(j)*ujk)/(ltilde(i) + ltilde(j));
	}
}

void BFF::convertDirichletToNeumann(const DenseMatrix& phi, DenseMatrix& g,
									DenseMatrix& h, bool surfaceHasCut)
{
	DenseMatrix a;
	DenseMatrix rhs = phi - data->Aib*g;
	data->Aii.L.solvePositiveDefinite(a, rhs);
	if (surfaceHasCut) processCut(vcat(a, g), a, g);

	h = -(data->Aib.transpose()*a + data->Abb*g);
}

void BFF::convertNeumannToDirichlet(const DenseMatrix& phi, const DenseMatrix& h,
									DenseMatrix& g)
{
	DenseMatrix a;
	DenseMatrix rhs = vcat(phi, -h);
	data->A.L.solvePositiveDefinite(a, rhs);

	g = a.submatrix(data->iN, data->N);
}

double BFF::computeTargetBoundaryLengths(const DenseMatrix& u, DenseMatrix& lstar) const
{
	double sum = 0.0;
	lstar = DenseMatrix(data->bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];
		int j = data->bIndex[w->prev()];

		lstar(i) = exp(0.5*(u(i) + u(j)))*data->l(i);
		sum += lstar(i);
	}

	return sum;
}

double BFF::computeTargetDualBoundaryLengths(const DenseMatrix& lstar,
											 DenseMatrix& ldual) const
{
	double sum = 0.0;
	ldual = DenseMatrix(data->bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];
		int j = data->bIndex[w->prev()];

		ldual(j) = 0.5*(lstar(i) + lstar(j));
		sum += ldual(j);
	}

	return sum;
}

double BFF::computeTargetBoundaryLengthsUV(DenseMatrix& lstar) const
{
	double sum = 0.0;
	lstar = DenseMatrix(data->bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];

		lstar(i) = (w->uv - w->prev()->uv).norm();
		sum += lstar(i);
	}

	return sum;
}

double BFF::computeTargetDualBoundaryLengthsUV(DenseMatrix& ldual) const
{
	double sum = 0.0;
	ldual = DenseMatrix(data->bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int j = data->bIndex[w->prev()];

		Vector uvi = w->nextWedge()->uv;
		Vector uvj = w->uv;
		Vector uvk = w->prev()->uv;
		ldual(j) = 0.5*((uvj - uvi).norm() + (uvk - uvj).norm());
		sum += ldual(j);
	}

	return sum;
}

void BFF::closeCurvatures(DenseMatrix& ktilde) const
{
	double L = 0.0;
	double totalAngle = 0.0;
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];

		L += data->l(i);
		totalAngle += ktilde(i);
	}

	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];
		int j = data->bIndex[w->prev()];

		double ldual = 0.5*(data->l(i) + data->l(j));
		ktilde(j) += (ldual/L)*(2*M_PI - totalAngle);
	}
}

void invert2x2(DenseMatrix& m)
{
	double det = m(0, 0)*m(1, 1) - m(0, 1)*m(1, 0);

	std::swap(m(0, 0), m(1, 1));
	m(0, 1) = -m(0, 1);
	m(1, 0) = -m(1, 0);
	m *= 1.0/det;
}

void BFF::closeLengths(const DenseMatrix& lstar, const DenseMatrix& Ttilde,
					   DenseMatrix& ltilde) const
{
	// to ensure maps are seamless across a cut, assign only a single degree of freedom,
	// i.e., a unique length to each pair of corresponding cut edges
	int eN = 0; // counter to track the number of unique edges along the cut boundary
	EdgeData<int> indexMap(mesh, -1); // assign a unique index to wedges along
									  // the boundary and a shared index to
									  // wedges on opposite sides of a cut
	for (WedgeCIter w: mesh.cutBoundary()) {
		if (indexMap[w->halfEdge()->next()->edge()] == -1) {
			indexMap[w->halfEdge()->next()->edge()] = eN++;
		}
	}

	// accumulate the diagonal entries of the mass matrix and the tangents
	DenseMatrix L(eN), diagNinv(eN), T(2, eN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];
		int ii = indexMap[w->halfEdge()->next()->edge()];

		L(ii) = lstar(i);
		diagNinv(ii) += 1.0/data->l(i);
		T(0, ii) += Ttilde(0, i);
		T(1, ii) += Ttilde(1, i);
	}

	for (int i = 0; i < eN; i++) {
		diagNinv(i) = 1.0/diagNinv(i);
	}

	// modify the target lengths to ensure gamma closes
	SparseMatrix Ninv = SparseMatrix::diag(diagNinv);
	DenseMatrix TT = T.transpose();
	DenseMatrix m = T*(Ninv*TT);
	invert2x2(m);
	L -= Ninv*(TT*(m*(T*L)));

	// copy the modified lengths into ltilde
	ltilde = DenseMatrix(data->bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];
		int ii = indexMap[w->halfEdge()->next()->edge()];

		ltilde(i) = L(ii);
	}
}

void BFF::constructBestFitCurve(const DenseMatrix& lstar, const DenseMatrix& ktilde,
								DenseMatrix& gammaRe, DenseMatrix& gammaIm) const
{
	// compute tangents as cumulative sum of angles phi
	double phi = 0.0;
	DenseMatrix Ttilde(2, data->bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];

		phi += ktilde(i);
		Ttilde(0, i) = cos(phi);
		Ttilde(1, i) = sin(phi);
	}

	// modify target lengths lstar to ensure gamma closes
	DenseMatrix ltilde;
	closeLengths(lstar, Ttilde, ltilde);

	// compute gamma as cumulative sum of products ltilde*Ttilde
	double re = 0.0;
	double im = 0.0;
	gammaRe = DenseMatrix(data->bN);
	gammaIm = DenseMatrix(data->bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = data->bIndex[w];

		gammaRe(i) = re;
		gammaIm(i) = im;
		re += ltilde(i)*Ttilde(0, i);
		im += ltilde(i)*Ttilde(1, i);
	}
}

void BFF::extendHarmonic(const DenseMatrix& g, DenseMatrix& h)
{
	DenseMatrix a;
	DenseMatrix rhs = -(data->Aib*g);
	data->Aii.L.solvePositiveDefinite(a, rhs);

	h = vcat(a, g);
}

void BFF::extendCurve(const DenseMatrix& gammaRe, const DenseMatrix& gammaIm,
					  DenseMatrix& a, DenseMatrix& b, bool conjugate)
{
	// extend real component of gamma
	extendHarmonic(gammaRe, a);

	if (conjugate) {
		// conjugate imaginary component of gamma
		DenseMatrix h(data->N);
		for (WedgeCIter w: mesh.cutBoundary()) {
			int i = data->index[w->prev()];
			int j = data->index[w];
			int k = data->index[w->nextWedge()];

			h(j) = 0.5*(a(k) - a(i));
		}

		data->A.L.solvePositiveDefinite(b, h);

	} else {
		// extend imaginary component of gamma
		extendHarmonic(gammaIm, b);
	}
}

void BFF::normalize()
{
	// compute center of mass
	Vector cm;
	int wN = 0;
	for (WedgeIter w = mesh.wedges().begin(); w != mesh.wedges().end(); w++) {
		if (w->isReal()) {
			std::swap(w->uv.x, w->uv.y);
			w->uv.x = -w->uv.x;

			cm += w->uv;
			wN++;
		}
	}
	cm /= wN;

	// translate to origin and determine radius
	for (WedgeIter w = mesh.wedges().begin(); w != mesh.wedges().end(); w++) {
		if (w->isReal()) {
			w->uv -= cm;
		}
	}

	// zero out nan entries
	for (WedgeIter w = mesh.wedges().begin(); w != mesh.wedges().end(); w++) {
		if (std::isnan(w->uv.x) || std::isnan(w->uv.y)) {
			w->uv.x = 0.0;
			w->uv.y = 0.0;
		}
	}
}

void BFF::flatten(const DenseMatrix& u, const DenseMatrix& ktilde, bool conjugate)
{
	// compute target lengths
	DenseMatrix lstar;
	computeTargetBoundaryLengths(u, lstar);

	// construct best fit curve gamma
	DenseMatrix gammaRe, gammaIm;
	constructBestFitCurve(lstar, ktilde, gammaRe, gammaIm);

	// extend
	DenseMatrix flatteningRe, flatteningIm;
	extendCurve(gammaRe, gammaIm, flatteningRe, flatteningIm, conjugate);

	// set uv coordinates
	for (WedgeIter w = mesh.wedges().begin(); w != mesh.wedges().end(); w++) {
		if (w->isReal()) {
			int i = data->index[w];

			w->uv.x = -flatteningRe(i); // minus sign accounts for clockwise boundary traversal
			w->uv.y = flatteningIm(i);
			w->uv.z = 0.0;
		}
	}

	normalize();
}

void BFF::flatten(DenseMatrix& target, bool givenScaleFactors)
{
	if (givenScaleFactors) {
		// compute mean scaling
		meanScaling = target.mean();

		// compute normal derivative of boundary scale factors
		DenseMatrix dudn;
		convertDirichletToNeumann(-data->K, target, dudn);

		// compute target boundary curvatures
		compatibleTarget = data->k - dudn;

		// flatten with scale factors u and compatible curvatures ktilde
		flatten(target, compatibleTarget, true);

	} else {
		// given target boundary curvatures, compute target boundary scale factors
		convertNeumannToDirichlet(-data->K, data->k - target, compatibleTarget);

		// the scale factors provided by the user and those computed from the neumann
		// to dirichlet map might differ by a constant, so adjust these scale factors by
		// this constant. Note: this is done solely to prevent the "jump" in scaling when
		// switching from curvatures to scale factors during direct editing
		double constantOffset = meanScaling - compatibleTarget.mean();
		for (int i = 0; i < data->bN; i++) compatibleTarget(i) += constantOffset;

		// flatten with compatible target scale factors u and curvatures ktilde
		flatten(compatibleTarget, target, false);
	}
}

void BFF::flattenWithCones(const DenseMatrix& C, bool surfaceHasNewCut)
{
	if (surfaceHasNewCut) cutSurfaceData = std::shared_ptr<BFFData>(new BFFData(mesh));

	// set boundary scale factors to zero. Note: scale factors can be assigned values
	// other than zero
	DenseMatrix u(data->bN);

	// compute normal derivative of boundary scale factors
	DenseMatrix dudn;
	convertDirichletToNeumann(-(data->K - C), u, dudn, true);

	// compute target boundary curvatures
	DenseMatrix ktilde = data->k - dudn;

	// flatten with compatible target scale factors u and curvatures ktilde
	flatten(u, ktilde, false);

	// reset current data to the data of the input surface
	data = inputSurfaceData;
}

void BFF::flattenToDisk()
{
	DenseMatrix u(data->bN), ktilde(data->bN);
	for (int iter = 0; iter < 10; iter++) {
		// compute target dual boundary edge lengths
		double L;
		DenseMatrix lstar, ldual;
		computeTargetBoundaryLengths(u, lstar);
		L = computeTargetDualBoundaryLengths(lstar, ldual);

		// set ktilde proportional to the most recent dual lengths
		for (WedgeCIter w: mesh.cutBoundary()) {
			int i = data->bIndex[w];

			ktilde(i) = 2*M_PI*ldual(i)/L;
		}

		// compute target scale factors
		convertNeumannToDirichlet(-data->K, data->k - ktilde, u);
	}

	// flatten with compatible target scale factors u and curvatures ktilde
	flatten(u, ktilde, false);
}

int sample(const std::vector<Vector>& gamma,
		   const std::vector<double>& cumulativeLengthGamma,
		   double t, int i, Vector& z) {
	while (cumulativeLengthGamma[i] < t) i++;

	// clamp i if there is numerical badness
	int n = (int)cumulativeLengthGamma.size();
	if (i == n) {
		i--;
		t = cumulativeLengthGamma[i];
	}

	double lprev = i > 0 ? cumulativeLengthGamma[i - 1] : 0.0;

	// sample via linear interpolation
	Vector gi = gamma[i];
	Vector gj = gamma[(i+1)%n];
	double tau = (t - lprev)/(cumulativeLengthGamma[i] - lprev);
	z = (1.0 - tau)*gi + tau*gj;

	return i;
}

double angle(const Vector& a, const Vector& b, const Vector& c) {
	Vector u = b - a;
	u.normalize();

	Vector v = c - b;
	v.normalize();

	double theta = atan2(v.y, v.x) - atan2(u.y, u.x);
	while (theta >= M_PI) theta -= 2*M_PI;
	while (theta < -M_PI) theta += 2*M_PI;

	return theta;
}

void BFF::flattenToShape(const std::vector<Vector>& gamma)
{
	int n = (int)gamma.size(); // number of vertices in target curve
	int nB = (int)data->bN; // number of vertices on domain boundary

	// Compute total and cumulative lengths of gamma, where
	// the value of cumulativeLengthGamma[i] is equal to the
	// total length of the piecewise linear curve up to
	// vertex i+1. In particular, this means the first entry
	// cumulativeLengthGamma[0] will be nonzero, and the final
	// entry will be the length of the entire curve.
	double L = 0.0;
	std::vector<double> cumulativeLengthGamma(n);
	for (int i = 0; i < n; i++) {
		int j = (i+1) % n;

		L += (gamma[j] - gamma[i]).norm();
		cumulativeLengthGamma[i] = L;
	}

	DenseMatrix u(nB);
	DenseMatrix kprev(nB);
	DenseMatrix ktilde = data->k;
	for (int iter = 0; iter < 10; iter++) {
		// compute target boundary edge lengths
		double S;
		DenseMatrix lstar;
		if (iter == 0) S = computeTargetBoundaryLengths(u, lstar);
		else S = computeTargetBoundaryLengthsUV(lstar);

		// sample vertices zi = gamma((L/S)si)
		double s = 0.0;
		int index = 0;
		std::vector<Vector> z(nB);
		for (WedgeCIter w: mesh.cutBoundary()) {
			int i = data->bIndex[w];

			double t = L*s/S;
			index = sample(gamma, cumulativeLengthGamma, t, index, z[i]);
			s += lstar(i);
		}

		// compute ktilde, which is the (integrated) curvature of the sampled curve z
		double sum = 0.0;
		for (WedgeCIter w: mesh.cutBoundary()) {
			int i = data->bIndex[w->nextWedge()];
			int j = data->bIndex[w];
			int k = data->bIndex[w->prev()];

			kprev(j) = ktilde(j); // store the previous solution for later use
			ktilde(j) = angle(z[i], z[j], z[k]);
			sum += ktilde(j);
		}

		// flip signs in case gamma has clockwise orientation
		if (sum < 0.0) ktilde = -ktilde;

		// stabilize iterations by averaging with ktilde from
		// previous iteration
		for (WedgeCIter w: mesh.cutBoundary()) {
			int i = data->bIndex[w];

			ktilde(i) = 0.5*(ktilde(i) + kprev(i));
		}

		if (iter == 0) closeCurvatures(ktilde);

		// compute target scale factors
		convertNeumannToDirichlet(-data->K, data->k - ktilde, u);

		// flatten with compatible target scale factors u and curvatures ktilde
		flatten(u, ktilde, false);
	}
}

void BFF::projectStereographically(VertexCIter pole, double radius,
								   const VertexData<Vector>& uvs)
{
	for (VertexIter v = mesh.vertices.begin(); v != mesh.vertices.end(); v++) {
		Vector projection(0, 1, 0);
		if (v != pole) {
			const Vector& uv = uvs[v];
			double X = uv.x*radius;
			double Y = uv.y*radius;

			projection = Vector(2*X, -1 + X*X + Y*Y, 2*Y)/(1 + X*X + Y*Y);
		}

		// set uv coordinates
		HalfEdgeIter he = v->halfEdge();
		do {
			he->next()->wedge()->uv = projection;

			he = he->flip()->next();
		} while (he != v->halfEdge());
	}
}

void BFF::mapToSphere()
{
	// remove an arbitrary vertex star
	VertexIter pole;
	for (VertexIter v = mesh.vertices.begin(); v != mesh.vertices.end(); v++) {
		if (!v->onBoundary()) {
			pole = v;
			break;
		}
	}

	pole->inNorthPoleVicinity = true;
	HalfEdgeIter he = pole->halfEdge();
	do {
		he->face()->inNorthPoleVicinity = true;

		HalfEdgeIter next = he->next();
		next->edge()->onCut = true;
		next->edge()->setHalfEdge(next);

		he->wedge()->inNorthPoleVicinity = true;
		next->wedge()->inNorthPoleVicinity = true;
		next->next()->wedge()->inNorthPoleVicinity = true;

		he = he->flip()->next();
	} while (he != pole->halfEdge());

	// initialize data class for this new surface without the vertex star
	std::shared_ptr<BFFData> sphericalSurfaceData(new BFFData(mesh));
	data = sphericalSurfaceData;

	// flatten this surface to a disk
	flattenToDisk();

	// stereographically project the disk to a sphere
	// since we do not know beforehand what the radius of our disk
	// should be to minimize area distortion, we perform a ternary search
	// to determine its radius
	VertexData<Vector> uvs(mesh);
	for (VertexIter v = mesh.vertices.begin(); v != mesh.vertices.end(); v++) {
		if (v != pole) uvs[v] = v->wedge()->uv;
	}

	double minRadius = 1.0;
	double maxRadius = 1000.0;
	do {
		double leftThird = minRadius + (maxRadius - minRadius)/3;
		double rightThird = maxRadius - (maxRadius - minRadius)/3;

		projectStereographically(pole, leftThird, uvs);
		double minDistortion = Distortion::computeAreaScaling(mesh.faces)[2];

		projectStereographically(pole, rightThird, uvs);
		double maxDistortion = Distortion::computeAreaScaling(mesh.faces)[2];

		if (minDistortion < maxDistortion) minRadius = leftThird;
		else maxRadius = rightThird;

		if (std::abs(maxDistortion - minDistortion) < 1e-3 ||
			std::isnan(minDistortion) || std::isnan(maxDistortion)) break;
	} while (true);

	// restore vertex star
	pole->inNorthPoleVicinity = false;
	he = pole->halfEdge();
	do {
		he->face()->inNorthPoleVicinity = false;

		HalfEdgeIter next = he->next();
		next->edge()->onCut = false;

		he->wedge()->inNorthPoleVicinity = false;
		next->wedge()->inNorthPoleVicinity = false;
		next->next()->wedge()->inNorthPoleVicinity = false;

		he = he->flip()->next();
	} while (he != pole->halfEdge());

	// reset current data to the data of the input surface
	data = inputSurfaceData;
}

BFFData::BFFData(Mesh& mesh_):
iN(0),
bN(0),
N(0),
index(mesh_),
bIndex(mesh_),
mesh(mesh_)
{
	init();
}

void BFFData::indexWedges()
{
	// index interior wedges
	iN = 0;
	for (VertexCIter v = mesh.vertices.begin(); v != mesh.vertices.end(); v++) {
		if (!v->onBoundary()) {
			HalfEdgeCIter he = v->halfEdge();
			do {
				WedgeCIter w = he->next()->wedge();
				bIndex[w] = -1;
				index[w] = iN;

				he = he->flip()->next();
			} while (he != v->halfEdge());

			iN++;
		}
	}

	// index boundary wedges
	bN = 0;
	for (WedgeCIter w: mesh.cutBoundary()) {
		HalfEdgeCIter he = w->halfEdge()->prev();
		do {
			WedgeCIter w = he->next()->wedge();
			bIndex[w] = bN;
			index[w] = iN + bN;

			if (he->edge()->onCut) break;
			he = he->flip()->next();
		} while (!he->onBoundary);

		bN++;
	}

	N = iN + bN;
}

void BFFData::computeIntegratedCurvatures()
{
	// compute integrated gaussian curvature
	K = DenseMatrix(iN);
	for (VertexCIter v = mesh.vertices.begin(); v != mesh.vertices.end(); v++) {
		if (!v->onBoundary()) {
			int i = index[v->wedge()];

			K(i) = v->angleDefect();
		}
	}

	// compute integrated geodesic curvature
	k = DenseMatrix(bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = bIndex[w];

		k(i) = w->exteriorAngle();
	}
}

void BFFData::computeBoundaryLengths()
{
	l = DenseMatrix(bN);
	for (WedgeCIter w: mesh.cutBoundary()) {
		int i = bIndex[w];

		l(i) = w->halfEdge()->next()->edge()->length();
	}
}

void BFFData::buildLaplace()
{
	Triplet T(N, N);
	for (FaceCIter f = mesh.faces.begin(); f != mesh.faces.end(); f++) {
		if (f->isReal()) {
			HalfEdgeCIter he = f->halfEdge();
			do {
				int i = index[he->next()->wedge()];
				int j = index[he->prev()->wedge()];
				double w = 0.5*he->cotan();

				T.add(i, i, w);
				T.add(j, j, w);
				T.add(i, j, -w);
				T.add(j, i, -w);

				he = he->next();
			} while (he != f->halfEdge());
		}
	}

	A = SparseMatrix(T);
	A += SparseMatrix::identity(N, N)*1e-8;
}

void BFFData::init()
{
	// assign indices to wedges
	indexWedges();

	// compute integrated gaussian and geodesic curvatures
	computeIntegratedCurvatures();

	// computes boundary edge lengths
	computeBoundaryLengths();

	// build laplace matrix and extract submatrices
	buildLaplace();
	Aii = A.submatrix(0, iN, 0, iN);
	Aib = A.submatrix(0, iN, iN, N);
	Abb = A.submatrix(iN, N, iN, N);
}

} // namespace bff
